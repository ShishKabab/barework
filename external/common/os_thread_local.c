#include "os.h"
#include <pthread.h>
#include <stdlib.h>

struct os_thread_local { pthread_key_t native; thread_function cleanup; };
os_thread_local_create_result OsThreadLocalCreate(thread_function cleanup, os_thread_local **output)
{
    if (!output || !cleanup) return (os_thread_local_create_result){.tag = OS_THREAD_LOCAL_CREATE_INVALID_ARGUMENT};
    *output = NULL;
    os_thread_local *key = malloc(sizeof(*key));
    if (!key) return (os_thread_local_create_result){.tag = OS_THREAD_LOCAL_CREATE_NO_SPACE};
    i32 error = pthread_key_create(&key->native, cleanup);
    if (error) { free(key); return (os_thread_local_create_result){.tag = OS_THREAD_LOCAL_CREATE_SYSTEM_ERROR, .native_error = error}; }
    key->cleanup = cleanup;
    *output = key;
    return (os_thread_local_create_result){.tag = OS_THREAD_LOCAL_CREATE_OK};
}
void *OsThreadLocalGet(os_thread_local *key) { return key ? pthread_getspecific(key->native) : NULL; }
os_thread_local_set_result OsThreadLocalSet(os_thread_local *key, void *value)
{
    if (!key) return (os_thread_local_set_result){.tag = OS_THREAD_LOCAL_SET_INVALID_ARGUMENT};
    i32 error = pthread_setspecific(key->native, value);
    return (os_thread_local_set_result){.tag = error ? OS_THREAD_LOCAL_SET_SYSTEM_ERROR : OS_THREAD_LOCAL_SET_OK, .native_error = error};
}
void OsThreadLocalDestroy(os_thread_local *key)
{
    if (!key) return;
    void *value = pthread_getspecific(key->native);
    /* With a live key these operations cannot fail. EINVAL would mean the
     * ownership/lifetime contract was violated; do not continue with dangling TLS. */
    if (pthread_setspecific(key->native, NULL) != 0) abort();
    if (value) key->cleanup(value);
    if (pthread_key_delete(key->native) != 0) abort();
    free(key);
}
