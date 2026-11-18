#include "wasm_reset.h"
#include "slice.h"

typedef struct cursor { const_byte_slice bytes; usize offset; } cursor;
typedef enum read_byte_tag { READ_BYTE_OK, READ_BYTE_END } read_byte_tag;
typedef struct read_byte_result { read_byte_tag tag; u8 value; } read_byte_result;
static read_byte_result ReadByte(cursor *input)
{
    if (input->offset >= input->bytes.len) return (read_byte_result){.tag = READ_BYTE_END};
    return (read_byte_result){.tag = READ_BYTE_OK, .value = input->bytes.ptr[input->offset++]};
}
typedef enum read_u32_tag { READ_U32_OK, READ_U32_INVALID } read_u32_tag;
typedef struct read_u32_result { read_u32_tag tag; u32 value; } read_u32_result;
static read_u32_result ReadU32(cursor *input)
{
    u32 value = 0;
    for (u32 shift = 0; shift < 35; shift += 7) {
        read_byte_result byte = ReadByte(input);
        if (byte.tag != READ_BYTE_OK || (shift == 28 && byte.value > 15)) break;
        value |= (u32)(byte.value & 127) << shift;
        if (!(byte.value & 128)) return (read_u32_result){.tag = READ_U32_OK, .value = value};
    }
    return (read_u32_result){.tag = READ_U32_INVALID};
}
static bool ByteEquals(cursor *input, u8 expected)
{
    read_byte_result read = ReadByte(input);
    return read.tag == READ_BYTE_OK && read.value == expected;
}
static bool NumberEquals(cursor *input, u32 expected)
{
    read_u32_result read = ReadU32(input);
    return read.tag == READ_U32_OK && read.value == expected;
}
static bool NumericTypes(cursor *input)
{
    read_u32_result count = ReadU32(input);
    if (count.tag != READ_U32_OK || count.value > input->bytes.len) return false;
    for (u32 i = 0; i < count.value; ++i) {
        read_byte_result type = ReadByte(input);
        if (type.tag != READ_BYTE_OK || type.value < 0x7b || type.value > 0x7f) return false;
    }
    return true;
}
static bool ConstantI32(cursor *input)
{
    return ByteEquals(input, 0x41) && ReadU32(input).tag == READ_U32_OK && ByteEquals(input, 0x0b);
}
static bool NameEquals(const_byte_slice name, string_slice text)
{
    if (name.len != text.len) return false;
    SliceFor(name, index) { if (name.ptr[index] != (u8)text.ptr[index]) return false; }
    return true;
}
bool WasmResetCompatible(const_byte_slice module)
{
    const u8 magic[] = {0, 'a', 's', 'm', 1, 0, 0, 0};
    const_byte_slice header = {magic, sizeof(magic)};
    if (!module.ptr || module.len < header.len) return false;
    SliceFor(header, index) { if (module.ptr[index] != header.ptr[index]) return false; }
    cursor input = {module, header.len};
    bool memory = false, table = false, global = false, memory_export = false, table_export = false, global_export = false;
    while (input.offset < input.bytes.len) {
        read_byte_result id = ReadByte(&input);
        read_u32_result length = ReadU32(&input);
        cursor section = {0};
        if (id.tag != READ_BYTE_OK || length.tag != READ_U32_OK ||
            ConstByteSliceSubslice(input.bytes, input.offset, length.value, &section.bytes).tag != CONST_BYTE_SLICE_SUBSLICE_OK) return false;
        input.offset += length.value;
        switch (id.value) {
        case 0: case 3: case 10: continue;
        case 1: {
            read_u32_result count = ReadU32(&section);
            if (count.tag != READ_U32_OK || count.value > section.bytes.len) return false;
            for (u32 i = 0; i < count.value; ++i)
                if (!ByteEquals(&section, 0x60) || !NumericTypes(&section) || !NumericTypes(&section)) return false;
            break;
        }
        case 2: case 9:
            if (!NumberEquals(&section, 0)) return false;
            break;
        case 4: {
            if (!NumberEquals(&section, 1) || !ByteEquals(&section, 0x70) || !NumberEquals(&section, 1)) return false;
            read_u32_result minimum = ReadU32(&section), maximum = ReadU32(&section);
            if (minimum.tag != READ_U32_OK || maximum.tag != READ_U32_OK || minimum.value != maximum.value || maximum.value > 64) return false;
            table = true;
            break;
        }
        case 5: {
            if (!NumberEquals(&section, 1) || !NumberEquals(&section, 1)) return false;
            read_u32_result minimum = ReadU32(&section), maximum = ReadU32(&section);
            if (minimum.tag != READ_U32_OK || maximum.tag != READ_U32_OK || minimum.value > maximum.value || maximum.value > 32) return false;
            memory = true;
            break;
        }
        case 6:
            if (!NumberEquals(&section, 1) || !ByteEquals(&section, 0x7f) || !ByteEquals(&section, 1) || !ConstantI32(&section)) return false;
            global = true;
            break;
        case 7: {
            read_u32_result count = ReadU32(&section);
            if (count.tag != READ_U32_OK || count.value > section.bytes.len) return false;
            for (u32 i = 0; i < count.value; ++i) {
                read_u32_result size = ReadU32(&section);
                const_byte_slice name;
                if (size.tag != READ_U32_OK || ConstByteSliceSubslice(section.bytes, section.offset, size.value, &name).tag != CONST_BYTE_SLICE_SUBSLICE_OK) return false;
                section.offset += size.value;
                read_byte_result kind = ReadByte(&section);
                read_u32_result index = ReadU32(&section);
                if (kind.tag != READ_BYTE_OK || index.tag != READ_U32_OK) return false;
                if (index.value == 0) {
                    if (kind.value == 2 && NameEquals(name, StringSlice("memory"))) memory_export = true;
                    if (kind.value == 1 && NameEquals(name, StringSlice("__indirect_function_table"))) table_export = true;
                    if (kind.value == 3 && NameEquals(name, StringSlice("__stack_pointer"))) global_export = true;
                }
            }
            break;
        }
        case 11: {
            read_u32_result count = ReadU32(&section);
            if (count.tag != READ_U32_OK || count.value > section.bytes.len) return false;
            for (u32 i = 0; i < count.value; ++i) {
                if (!NumberEquals(&section, 0) || !ConstantI32(&section)) return false;
                read_u32_result size = ReadU32(&section);
                const_byte_slice data;
                if (size.tag != READ_U32_OK || ConstByteSliceSubslice(section.bytes, section.offset, size.value, &data).tag != CONST_BYTE_SLICE_SUBSLICE_OK) return false;
                section.offset += data.len;
            }
            break;
        }
        default: return false;
        }
        if (section.offset != section.bytes.len) return false;
    }
    return memory && table && global && memory_export && table_export && global_export;
}
