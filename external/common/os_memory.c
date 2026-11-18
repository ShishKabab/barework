#define _GNU_SOURCE
#include "os.h"

#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

os_memory_page_size_result OsMemoryPageSize(void)
{
    long size = sysconf(_SC_PAGESIZE);
    if (size <= 0) return (os_memory_page_size_result){.tag = OS_MEMORY_PAGE_SIZE_SYSTEM_ERROR, .native_error = errno};
    return (os_memory_page_size_result){.tag = OS_MEMORY_PAGE_SIZE_OK, .value = (usize)size};
}

static bool OsMemoryValidPages(byte_slice pages)
{
    os_memory_page_size_result queried = OsMemoryPageSize();
    if (queried.tag != OS_MEMORY_PAGE_SIZE_OK) return false;
    usize page_size = queried.value;
    return page_size != 0 && pages.ptr != NULL && pages.len != 0 &&
           (uintptr_t)pages.ptr % page_size == 0 && pages.len % page_size == 0 &&
           pages.len <= UINTPTR_MAX - (uintptr_t)pages.ptr;
}

os_memory_reserve_result OsMemoryReserve(usize size, byte_slice *reservation)
{
    if (reservation == NULL) {
        return (os_memory_reserve_result){.tag = OS_MEMORY_RESERVE_INVALID_ARGUMENT};
    }
    *reservation = (byte_slice){0};
    os_memory_page_size_result queried = OsMemoryPageSize();
    if (queried.tag != OS_MEMORY_PAGE_SIZE_OK)
        return (os_memory_reserve_result){.tag = OS_MEMORY_RESERVE_SYSTEM_ERROR, .native_error = queried.native_error};
    usize page_size = queried.value;
    if (size == 0 || page_size == 0) {
        return (os_memory_reserve_result){.tag = OS_MEMORY_RESERVE_INVALID_ARGUMENT};
    }
    if (size > PTRDIFF_MAX || size > SIZE_MAX - (page_size - 1)) {
        return (os_memory_reserve_result){.tag = OS_MEMORY_RESERVE_OUT_OF_MEMORY};
    }
    usize length = ((size + page_size - 1) / page_size) * page_size;
    void *memory = mmap(NULL, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        return (os_memory_reserve_result){.tag = OS_MEMORY_RESERVE_SYSTEM_ERROR, .native_error = errno};
    }
    *reservation = (byte_slice){memory, length};
    return (os_memory_reserve_result){.tag = OS_MEMORY_RESERVE_OK};
}

os_memory_commit_result OsMemoryCommit(byte_slice pages)
{
    if (!OsMemoryValidPages(pages)) {
        return (os_memory_commit_result){.tag = OS_MEMORY_COMMIT_INVALID_ARGUMENT};
    }
    if (mprotect(pages.ptr, pages.len, PROT_READ | PROT_WRITE) < 0)
        return (os_memory_commit_result){.tag = OS_MEMORY_COMMIT_SYSTEM_ERROR, .native_error = errno};
    return (os_memory_commit_result){.tag = OS_MEMORY_COMMIT_OK};
}

os_memory_decommit_result OsMemoryDecommit(byte_slice pages)
{
    if (!OsMemoryValidPages(pages)) {
        return (os_memory_decommit_result){.tag = OS_MEMORY_DECOMMIT_INVALID_ARGUMENT};
    }
    /* Replace only pages in our existing reservation. MAP_FIXED retains their
     * addresses while replacing their contents with inaccessible zero pages. */
    void *memory = mmap(pages.ptr, pages.len, PROT_NONE,
                        MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED)
        return (os_memory_decommit_result){.tag = OS_MEMORY_DECOMMIT_SYSTEM_ERROR, .native_error = errno};
    return (os_memory_decommit_result){.tag = OS_MEMORY_DECOMMIT_OK};
}

os_memory_free_result OsMemoryFree(byte_slice *reservation)
{
    if (reservation == NULL || !OsMemoryValidPages(*reservation)) {
        return (os_memory_free_result){.tag = OS_MEMORY_FREE_INVALID_ARGUMENT};
    }
    if (munmap(reservation->ptr, reservation->len) < 0) {
        return (os_memory_free_result){.tag = OS_MEMORY_FREE_SYSTEM_ERROR, .native_error = errno};
    }
    *reservation = (byte_slice){0};
    return (os_memory_free_result){.tag = OS_MEMORY_FREE_OK};
}
