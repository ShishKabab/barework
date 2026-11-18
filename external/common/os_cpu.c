#define _GNU_SOURCE
#include "os.h"
#include "slice.h"

#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

/* Paths and fscanf formats are confined to the libc/sysfs API boundary. */
typedef enum cpu_topology_id_tag { CPU_TOPOLOGY_ID_OK, CPU_TOPOLOGY_ID_UNAVAILABLE } cpu_topology_id_tag;
typedef struct cpu_topology_id_result { cpu_topology_id_tag tag; u32 value; } cpu_topology_id_result;
static cpu_topology_id_result CpuTopologyId(u32 cpu, string_slice field)
{
    char storage[160];
    string_slice path = {storage, sizeof(storage)};
    i32 length = snprintf(storage, path.len, "/sys/devices/system/cpu/cpu%u/topology/%.*s",
                          cpu, (int)field.len, field.ptr);
    if (length < 0 || (usize)length >= path.len)
        return (cpu_topology_id_result){.tag = CPU_TOPOLOGY_ID_UNAVAILABLE};
    FILE *file = fopen(path.ptr, "r");
    if (file == NULL) return (cpu_topology_id_result){.tag = CPU_TOPOLOGY_ID_UNAVAILABLE};
    int value;
    i32 valid = fscanf(file, "%d", &value) == 1 && value >= 0;
    fclose(file);
    if (!valid) return (cpu_topology_id_result){.tag = CPU_TOPOLOGY_ID_UNAVAILABLE};
    return (cpu_topology_id_result){.tag = CPU_TOPOLOGY_ID_OK, .value = (u32)value};
}

static void CpuClassMask(string_slice path, cpu_set_t *mask, usize size, usize capacity)
{
    CPU_ZERO_S(size, mask);
    char path_storage[160];
    byte_slice terminated = {(u8 *)path_storage, sizeof(path_storage)};
    if (path.len >= terminated.len ||
        ByteSliceCopy(terminated, 0, (const_byte_slice){(const u8 *)path.ptr, path.len}).tag != BYTE_SLICE_COPY_OK) return;
    terminated.ptr[path.len] = 0;
    FILE *file = fopen(path_storage, "r");
    if (file == NULL) {
        return;
    }
    unsigned first, last;
    while (fscanf(file, "%u", &first) == 1) {
        last = first;
        int delimiter = fgetc(file);
        if (delimiter == '-') {
            if (fscanf(file, "%u", &last) != 1) {
                CPU_ZERO_S(size, mask);
                break;
            }
            delimiter = fgetc(file);
        }
        if (first > last) {
            CPU_ZERO_S(size, mask);
            break;
        }
        for (usize cpu = first; cpu <= last && cpu < capacity; ++cpu) {
            CPU_SET_S(cpu, size, mask);
        }
        if (delimiter != ',') {
            break;
        }
    }
    fclose(file);
}

typedef struct cpu_identity { cpu_topology_id_result package; cpu_topology_id_result core; } cpu_identity;
typedef struct cpu_identity_slice { cpu_identity *ptr; usize len; } cpu_identity_slice;

os_cpu_info_result OsCpuInfo(os_cpu_info *info)
{
    if (info == NULL) {
        return (os_cpu_info_result){.tag = OS_CPU_INFO_INVALID_ARGUMENT};
    }
    *info = (os_cpu_info){0};
    usize capacity = 128;
    cpu_set_t *allowed = NULL;
    usize size;
    for (;;) {
        size = CPU_ALLOC_SIZE(capacity);
        allowed = CPU_ALLOC(capacity);
        if (allowed == NULL) {
            return (os_cpu_info_result){.tag = OS_CPU_INFO_OUT_OF_MEMORY};
        }
        if (sched_getaffinity(0, size, allowed) == 0) {
            break;
        }
        i32 error = errno;
        CPU_FREE(allowed);
        if (error != EINVAL || capacity > INT_MAX / 2) {
            return (os_cpu_info_result){.tag = OS_CPU_INFO_SYSTEM_ERROR, .native_error = error};
        }
        capacity *= 2;
    }
    cpu_set_t *performance = CPU_ALLOC(capacity);
    cpu_set_t *efficiency = CPU_ALLOC(capacity);
    cpu_identity_slice identities = {calloc(capacity, sizeof(cpu_identity)), capacity};
    u32 *core_cpus = calloc(capacity, sizeof(u32));
    u32 *performance_cpus = calloc(capacity, sizeof(u32));
    if (performance == NULL || efficiency == NULL || identities.ptr == NULL || core_cpus == NULL || performance_cpus == NULL) {
        CPU_FREE(allowed);
        CPU_FREE(performance);
        CPU_FREE(efficiency);
        free(identities.ptr);
        free(core_cpus);
        free(performance_cpus);
        return (os_cpu_info_result){.tag = OS_CPU_INFO_OUT_OF_MEMORY};
    }
    CpuClassMask(StringSlice("/sys/devices/cpu_core/cpus"), performance, size, capacity);
    CpuClassMask(StringSlice("/sys/devices/cpu_atom/cpus"), efficiency, size, capacity);
    SliceFor(identities, cpu) {
        identities.ptr[cpu] = (cpu_identity){.package.tag = CPU_TOPOLOGY_ID_UNAVAILABLE, .core.tag = CPU_TOPOLOGY_ID_UNAVAILABLE};
        if (!CPU_ISSET_S(cpu, size, allowed)) {
            continue;
        }
        ++info->logical_count;
        cpu_identity identity = {
            CpuTopologyId((u32)cpu, StringSlice("physical_package_id")),
            CpuTopologyId((u32)cpu, StringSlice("core_id")),
        };
        i32 known = identity.package.tag == CPU_TOPOLOGY_ID_OK && identity.core.tag == CPU_TOPOLOGY_ID_OK;
        i32 duplicate = 0;
        cpu_identity_slice previous = {identities.ptr, cpu};
        SliceFor(previous, index) {
            if (known && previous.ptr[index].package.tag == CPU_TOPOLOGY_ID_OK && previous.ptr[index].core.tag == CPU_TOPOLOGY_ID_OK && previous.ptr[index].package.value == identity.package.value &&
                previous.ptr[index].core.value == identity.core.value) {
                duplicate = 1;
                break;
            }
        }
        identities.ptr[cpu] = identity;
        if (duplicate) {
            continue;
        }
        core_cpus[info->core_count++] = (u32)cpu;
        if (!known) {
            ++info->unresolved_count;
        }
        if (CPU_ISSET_S(cpu, size, performance) && !CPU_ISSET_S(cpu, size, efficiency)) {
            performance_cpus[info->performance_count++] = (u32)cpu;
        } else if (CPU_ISSET_S(cpu, size, efficiency) && !CPU_ISSET_S(cpu, size, performance)) {
            ++info->efficiency_count;
        } else {
            ++info->unknown_count;
        }
    }
    CPU_FREE(allowed);
    CPU_FREE(performance);
    CPU_FREE(efficiency);
    free(identities.ptr);
    if (info->core_count == 0) {
        free(core_cpus);
        free(performance_cpus);
        *info = (os_cpu_info){0};
        return (os_cpu_info_result){.tag = OS_CPU_INFO_UNAVAILABLE};
    }
    info->core_cpus = (os_cpu_id_slice){core_cpus, info->core_count};
    info->performance_cpus = (os_cpu_id_slice){performance_cpus, info->performance_count};
    return (os_cpu_info_result){.tag = OS_CPU_INFO_OK};
}

void OsCpuInfoDestroy(os_cpu_info *info)
{
    if (info == NULL) return;
    free(info->core_cpus.ptr);
    free(info->performance_cpus.ptr);
    *info = (os_cpu_info){0};
}
