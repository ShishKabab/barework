#include "os.h"
#include <assert.h>

typedef struct thread_state { os_thread_local *key; u64 owner; usize cleaned; os_thread *thread; } thread_state;
static void Cleanup(void *context)
{
    thread_state *state = context;
    assert(state->owner == ThreadCurrentId());
    ++state->cleaned;
}
static void Run(void *context)
{
    thread_state *state = context;
    state->owner = ThreadCurrentId();
    assert(OsThreadLocalGet(state->key) == NULL);
    assert(OsThreadLocalSet(state->key, state).tag == OS_THREAD_LOCAL_SET_OK);
    assert(OsThreadLocalGet(state->key) == state);
}
int main(void)
{
    os_thread_local *key;
    assert(OsThreadLocalCreate(NULL, &key).tag == OS_THREAD_LOCAL_CREATE_INVALID_ARGUMENT);
    assert(OsThreadLocalCreate(Cleanup, NULL).tag == OS_THREAD_LOCAL_CREATE_INVALID_ARGUMENT);
    assert(OsThreadLocalSet(NULL, NULL).tag == OS_THREAD_LOCAL_SET_INVALID_ARGUMENT);
    assert(OsThreadLocalCreate(Cleanup, &key).tag == OS_THREAD_LOCAL_CREATE_OK);
    thread_state main_state = {.key = key, .owner = ThreadCurrentId()};
    assert(OsThreadLocalSet(key, &main_state).tag == OS_THREAD_LOCAL_SET_OK);
    thread_state storage[2] = {{.key = key}, {.key = key}};
    struct state_slice { thread_state *ptr; usize len; } states = {storage, 2};
    SliceFor(states, index) { assert(ThreadStart(Run, &states.ptr[index], (os_cpu_id_slice){0}, &states.ptr[index].thread).tag == THREAD_START_OK); }
    SliceFor(states, index) { assert(ThreadJoin(states.ptr[index].thread).tag == THREAD_JOIN_OK); assert(states.ptr[index].cleaned == 1); }
    assert(main_state.cleaned == 0 && OsThreadLocalGet(key) == &main_state);
    OsThreadLocalDestroy(key);
    assert(main_state.cleaned == 1);
    OsThreadLocalDestroy(NULL);
    return 0;
}
