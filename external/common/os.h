#ifndef COMMON_OS_H
#define COMMON_OS_H
#include "os_results.h"

#include "types.h"

/* Write to stdout without adding a newline. Reports a tag and accepted byte
 * count (including partial progress on failure). Does not retain or flush text. */
terminal_write_result TerminalWrite(string_slice text);
/* Flush stdout; native error codes are diagnostic payloads only. */
terminal_flush_result TerminalFlush(void);

/* Describe a positive native diagnostic code, not a project result tag.
 * Borrowed text is invalidated by later error-description calls. */
string_slice OsErrorText(i32 error);
/* Monotonic elapsed-time clock, in nanoseconds; output is valid on OK. */
os_monotonic_nanoseconds_result OsMonotonicNanoseconds(u64 *nanoseconds);

/* Host-only virtual memory. Each operation has its own tagged result.
 * Reserve rounds size up to whole pages and returns inaccessible address space.
 * Commit makes pages read/write; Linux still faults in physical pages lazily.
 * Decommit discards contents and makes pages inaccessible without releasing
 * their addresses. Recommit yields zeroed pages. Free releases the reservation.
 * Ranges must be nonempty, page-aligned, whole-page slices owned by the caller
 * within a live reservation. Free must receive the entire original reservation.
 * No concurrent accesses or pending I/O may reference changed/freed pages.
 * Do not substitute arbitrary pointers: ownership is a caller precondition. */
os_memory_page_size_result OsMemoryPageSize(void);
os_memory_reserve_result OsMemoryReserve(usize size, byte_slice *reservation);
os_memory_commit_result OsMemoryCommit(byte_slice pages);
os_memory_decommit_result OsMemoryDecommit(byte_slice pages);
os_memory_free_result OsMemoryFree(byte_slice *reservation);

typedef struct os_thread os_thread;
typedef void (*thread_function)(void *context);
typedef struct os_thread_local os_thread_local;
typedef enum os_thread_local_create_tag { OS_THREAD_LOCAL_CREATE_OK, OS_THREAD_LOCAL_CREATE_INVALID_ARGUMENT, OS_THREAD_LOCAL_CREATE_NO_SPACE, OS_THREAD_LOCAL_CREATE_SYSTEM_ERROR } os_thread_local_create_tag;
typedef struct os_thread_local_create_result { os_thread_local_create_tag tag; i32 native_error; } os_thread_local_create_result;
typedef enum os_thread_local_set_tag { OS_THREAD_LOCAL_SET_OK, OS_THREAD_LOCAL_SET_INVALID_ARGUMENT, OS_THREAD_LOCAL_SET_SYSTEM_ERROR } os_thread_local_set_tag;
typedef struct os_thread_local_set_result { os_thread_local_set_tag tag; i32 native_error; } os_thread_local_set_result;
/* Key is shared; each value belongs to its thread. cleanup runs on that thread
 * at exit. Join all other users before destroying the key; Destroy also cleans
 * the calling thread's value. Set replaces a value without cleaning the old one. */
os_thread_local_create_result OsThreadLocalCreate(thread_function cleanup, os_thread_local **output);
void *OsThreadLocalGet(os_thread_local *key);
os_thread_local_set_result OsThreadLocalSet(os_thread_local *key, void *value);
void OsThreadLocalDestroy(os_thread_local *key);
typedef struct os_cpu_id_slice { u32 *ptr; usize len; } os_cpu_id_slice;

/* Context must live until the thread exits.
 * A successful start transfers ownership of a joinable handle to the caller.
 * Do not share the handle between joiners or detach/cancel the thread.
 * Empty affinity inherits the caller's mask. Otherwise every CPU must be in
 * the caller's allowed mask; affinity is installed before the callback runs.
 * The CPU slice is borrowed only during this call. Failure creates no thread. */
thread_start_result ThreadStart(thread_function function, void *context, os_cpu_id_slice affinity, os_thread **thread);
/* Waits for exit and frees the handle on success; do not use it afterward. */
thread_join_result ThreadJoin(os_thread *thread);
/* Scheduling hint for retry loops; does not synchronize memory or guarantee
 * another thread runs. SPSC operations themselves never wait or yield. */
void ThreadYield(void);
/* Process-local OS thread identity, used to enforce pinned resource ownership. */
u64 ThreadCurrentId(void);

typedef struct os_cpu_info {
    u32 logical_count;
    u32 core_count;
    u32 performance_count;
    u32 efficiency_count;
    u32 unknown_count;
    u32 unresolved_count;
    os_cpu_id_slice core_cpus; /* One allowed logical CPU per physical core. */
    os_cpu_id_slice performance_cpus; /* Subset containing only known P-cores. */
} os_cpu_info;
/* Snapshot of CPUs available in the calling thread's affinity mask. Deduplicates
 * physical package/core IDs; missing IDs count as individual cores and increment
 * unresolved_count. P/E classification uses Intel hybrid PMU lists when exposed;
 * other/missing classifications are unknown (never guessed from frequency).
 * core_count = performance_count + efficiency_count + unknown_count.
 * Init into a fresh/destroyed info. On success owns both CPU slices, released
 * by OsCpuInfoDestroy. CPU IDs respect the caller's affinity/cpuset. */
os_cpu_info_result OsCpuInfo(os_cpu_info *info);
void OsCpuInfoDestroy(os_cpu_info *info);

/* Coalescing wakeups, not message storage. Signal after publishing queue data.
 * Wait blocks and consumes a notification; Consume is nonblocking. A notification
 * may represent many messages. Close only after waiters/producers have stopped. */
os_event_create_result OsEventCreate(i32 *event);
os_event_signal_result OsEventSignal(i32 event);
os_event_wait_result OsEventWait(i32 event);
os_event_consume_result OsEventConsume(i32 event);
void OsEventClose(i32 event);

typedef struct io_queue io_queue;

typedef enum io_completion_tag {
    IO_COMPLETION_OK, IO_COMPLETION_INTERRUPTED, IO_COMPLETION_WOULD_BLOCK,
    IO_COMPLETION_ABORTED, IO_COMPLETION_PROTOCOL_ERROR, IO_COMPLETION_BAD_HANDLE,
    IO_COMPLETION_SYSTEM_ERROR,
} io_completion_tag;
typedef struct io_completion {
    u64 id;
    io_completion_tag tag;
    union { u32 value; i32 native_error; }; /* value only for OK; receive zero is EOF. */
} io_completion;

/* Single-threaded queue. Each operation has its own tagged result.
 * No synchronous fallback. Completion tags are separate from queue-call tags. */
io_create_result IoCreate(u32 capacity, io_queue **queue);
/* Requires all queued operations completed; otherwise returns IO_DESTROY_BUSY. */
io_destroy_result IoDestroy(io_queue *queue);
/* Submits queued operations and waits for one completion. */
io_wait_result IoWait(io_queue *queue, io_completion *completion);
/* Submits queued operations, then checks for one completion without waiting.
 * IO_POLL_OK supplies a completion. IO_POLL_EMPTY leaves it unchanged,
 * including when the queue is empty. Other tags describe polling failures. */
io_poll_result IoPoll(io_queue *queue, io_completion *completion);
/* Queues a one-shot readiness notification; consume the event then rearm. */
io_event_wait_result IoEventWait(io_queue *queue, i32 event, u64 id);

/* IPv4 address and port are in host byte order. E.g. 0x7f000001 = loopback. */
tcp_listen_result TcpListen(u32 address, u16 port, i32 backlog);
/* Synchronous connect for native clients/tests; server I/O remains asynchronous.
 * Owns the returned socket only on OK. REFUSED is safe to retry. */
tcp_connect_result TcpConnect(u32 address, u16 port);
/* Queues one operation tagged with id. At most capacity operations may be
 * outstanding. Keep sockets and slice storage alive until their completion;
 * do not read/write receive storage or modify send storage while pending.
 * Send may complete partially; the caller must queue the remaining slice. */
tcp_accept_result TcpAccept(io_queue *queue, i32 listener, u64 id);
tcp_receive_result TcpReceive(io_queue *queue, i32 socket, byte_slice buffer, u64 id);
tcp_send_result TcpSend(io_queue *queue, i32 socket, const_byte_slice buffer, u64 id);
/* Only close sockets with no outstanding operations. */
tcp_close_result TcpClose(i32 socket);

#endif
