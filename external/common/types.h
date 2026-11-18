#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef float f32;
typedef double f64;

typedef size_t usize;
typedef ptrdiff_t isize;

/* Non-owning views. len counts elements; null ptr requires len == 0. */
typedef struct byte_slice {
    u8 *ptr;
    usize len;
} byte_slice;

typedef struct const_byte_slice {
    const u8 *ptr;
    usize len;
} const_byte_slice;

/* Non-owning, address-independent view into a separately supplied byte slice.
 * index and len count bytes. Protocols must specify the backing slice's origin
 * (for dispatch messages, the beginning of the complete message). Copying that
 * backing storage preserves the range. Resolve and validate before accessing.
 * An empty range may point at the end; {0, 0} is empty, not an absent value.
 * Fixed-width fields do not specify wire byte order or struct padding. */
typedef struct byte_range {
    u32 index;
    u32 len;
} byte_range;

/* Strings are length-delimited and need not be null-terminated. */
typedef struct string_slice {
    const char *ptr;
    usize len;
} string_slice;

/* Accepts a string literal or a null-terminated char array, never a pointer.
 * The final terminator is excluded; embedded null bytes are preserved. */
/* Brace form also works in strict-C static aggregate initializers. */
#define StringSliceInitializer(str) {(str), sizeof(str) - 1}
#define StringSlice(str) ((string_slice)StringSliceInitializer(str))

/* Iterate by index over any slice type. Evaluates the length once, supports
 * empty slices, and preserves normal break/continue behavior. Keep the slice
 * storage valid for the loop. The index and its bound are scoped to the loop. */
#define SliceFor(slice, index) \
    for (usize index = 0, index##_slice_len = (slice).len; \
         index < index##_slice_len; ++index)

#endif
