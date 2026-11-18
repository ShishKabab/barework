#define _POSIX_C_SOURCE 200809L
#include "bump.h"
#include "os.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

/* Fault probes run in a child so reserved/decommitted protection is tested
 * without crashing the runner. Disable core files for these expected faults. */
static byte_slice Allocate(bump_allocator *arena, usize size, usize alignment)
{
    bump_allocate_result result = BumpAllocate(arena, size, alignment);
    assert(result.tag == BUMP_ALLOCATE_OK);
    return result.allocation;
}
static void ExpectInaccessible(const_byte_slice range)
{
    assert(range.ptr != NULL && range.len != 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        struct rlimit limit = {0, 0};
        assert(setrlimit(RLIMIT_CORE, &limit) == 0);
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS, SIG_DFL);
        volatile u8 value = *(const volatile u8 *)range.ptr;
        (void)value;
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) &&
           (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS));
}

static void TestPages(void)
{
    os_memory_page_size_result queried = OsMemoryPageSize();
    assert(queried.tag == OS_MEMORY_PAGE_SIZE_OK);
    usize page = queried.value;
    assert(page != 0);
    byte_slice reservation = {0};
    assert(OsMemoryReserve(0, &reservation).tag == OS_MEMORY_RESERVE_INVALID_ARGUMENT);
    assert(OsMemoryReserve(SIZE_MAX, &reservation).tag == OS_MEMORY_RESERVE_OUT_OF_MEMORY);
    assert(OsMemoryReserve(page + 1, &reservation).tag == OS_MEMORY_RESERVE_OK);
    assert(reservation.len == page * 2 && (uintptr_t)reservation.ptr % page == 0);
    byte_slice first = {reservation.ptr, page};
    byte_slice second = {reservation.ptr + page, page};
    ExpectInaccessible((const_byte_slice){first.ptr, first.len});
    assert(OsMemoryCommit(first).tag == OS_MEMORY_COMMIT_OK);
    SliceFor(first, index) {
        assert(first.ptr[index] == 0);
    }
    memset(first.ptr, 0x5a, first.len);
    assert(OsMemoryCommit(first).tag == OS_MEMORY_COMMIT_OK);
    assert(first.ptr[0] == 0x5a); /* Commit is not destructive. */
    ExpectInaccessible((const_byte_slice){second.ptr, second.len});
    assert(OsMemoryCommit(second).tag == OS_MEMORY_COMMIT_OK);
    memset(second.ptr, 0x7b, second.len);
    assert(OsMemoryCommit((byte_slice){first.ptr + 1, page}).tag == OS_MEMORY_COMMIT_INVALID_ARGUMENT);
    assert(OsMemoryDecommit((byte_slice){first.ptr, page - 1}).tag == OS_MEMORY_DECOMMIT_INVALID_ARGUMENT);
    assert(OsMemoryDecommit(first).tag == OS_MEMORY_DECOMMIT_OK);
    ExpectInaccessible((const_byte_slice){first.ptr, first.len});
    assert(OsMemoryCommit(first).tag == OS_MEMORY_COMMIT_OK);
    SliceFor(first, index) {
        assert(first.ptr[index] == 0 && second.ptr[index] == 0x7b);
    }
    assert(OsMemoryFree(&reservation).tag == OS_MEMORY_FREE_OK);
    assert(reservation.ptr == NULL && reservation.len == 0);
    assert(OsMemoryFree(&reservation).tag == OS_MEMORY_FREE_INVALID_ARGUMENT);
}

static void TestArenas(void)
{
    os_memory_page_size_result queried = OsMemoryPageSize();
    assert(queried.tag == OS_MEMORY_PAGE_SIZE_OK);
    usize page = queried.value;
    bump_allocator arena;
    assert(BumpCreate(&arena, (usize)1024 * 1024 * 1024).tag == BUMP_CREATE_OK);
    assert(arena.used == 0 && arena.committed == 0);
    u8 *base = arena.storage.ptr;
    byte_slice first = Allocate(&arena, 17, 16);
    assert(first.ptr == base && first.len == 17 && arena.committed == page);
    memset(first.ptr, 0x55, first.len);
    byte_slice second = Allocate(&arena, page * 2, 64);
    assert(second.len == page * 2 && (uintptr_t)second.ptr % 64 == 0);
    assert(arena.storage.ptr == base && arena.committed == page * 3);
    SliceFor(first, index) {
        assert(first.ptr[index] == 0x55);
    }
    SliceFor(second, index) {
        assert(second.ptr[index] == 0);
    }
    usize used = arena.used;
    usize committed = arena.committed;
    assert(BumpAllocate(&arena, SIZE_MAX, 1).tag == BUMP_ALLOCATE_NO_SPACE);
    assert(arena.used == used && arena.committed == committed);
    ExpectInaccessible((const_byte_slice){base + committed, page});
    BumpReset(&arena);
    assert(arena.used == 0 && arena.committed == committed && arena.storage.ptr == base);
    assert(first.ptr[0] == 0x55); /* Reset does not erase retained storage. */
    byte_slice reused = Allocate(&arena, first.len, 1);
    assert(reused.ptr == first.ptr && reused.len == first.len);
    assert(BumpDestroy(&arena).tag == BUMP_DESTROY_OK && arena.storage.ptr == NULL);
    assert(BumpDestroy(&arena).tag == BUMP_DESTROY_OK);

    assert(BumpCreate(&arena, page + 3).tag == BUMP_CREATE_OK);
    assert(Allocate(&arena, page + 3, 1).len == page + 3);
    assert(arena.committed == page * 2);
    assert(BumpAllocate(&arena, 1, 1).tag == BUMP_ALLOCATE_NO_SPACE);
    assert(BumpDestroy(&arena).tag == BUMP_DESTROY_OK);

    u8 storage[16];
    assert(BumpInit(&arena, (byte_slice){storage, sizeof(storage)}).tag == BUMP_INIT_OK);
    assert(Allocate(&arena, sizeof(storage), 1).len == sizeof(storage));
    assert(BumpDestroy(&arena).tag == BUMP_DESTROY_OK); /* Borrowed stack memory is not unmapped. */
    storage[0] = 1;
    assert(storage[0] == 1);
}

int main(void)
{
    TestPages();
    TestArenas();
    string_slice message = StringSlice("PASS: virtual reserve/commit/decommit/free, protection, stable arenas\n");
    assert(fwrite(message.ptr, 1, message.len, stdout) == message.len);
    return 0;
}
