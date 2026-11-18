#pragma once
#include "types.h"

typedef enum os_memory_page_size_tag { OS_MEMORY_PAGE_SIZE_OK, OS_MEMORY_PAGE_SIZE_SYSTEM_ERROR } os_memory_page_size_tag;
typedef struct os_memory_page_size_result { os_memory_page_size_tag tag; usize value; i32 native_error; } os_memory_page_size_result;

typedef enum tcp_connect_tag { TCP_CONNECT_OK, TCP_CONNECT_REFUSED, TCP_CONNECT_SYSTEM_ERROR } tcp_connect_tag;
typedef struct tcp_connect_result {
    tcp_connect_tag tag;
    union { i32 socket; i32 native_error; };
} tcp_connect_result;

typedef enum terminal_write_tag { TERMINAL_WRITE_OK, TERMINAL_WRITE_SYSTEM_ERROR } terminal_write_tag;
typedef struct terminal_write_result {
    terminal_write_tag tag;
    usize written; /* Progress is valid for either tag. */
    i32 native_error; /* Only for SYSTEM_ERROR. */
} terminal_write_result;

/* Function-specific results. Inspect the tag before reading its payload.
 * native_error is meaningful only for SYSTEM_ERROR; it is diagnostic errno. */
typedef enum terminal_flush_tag {
    TERMINAL_FLUSH_OK, TERMINAL_FLUSH_SYSTEM_ERROR
} terminal_flush_tag;
typedef struct terminal_flush_result {
    terminal_flush_tag tag;
    i32 native_error;
} terminal_flush_result;

typedef enum os_monotonic_nanoseconds_tag {
    OS_MONOTONIC_NANOSECONDS_OK, OS_MONOTONIC_NANOSECONDS_INVALID_ARGUMENT, OS_MONOTONIC_NANOSECONDS_SYSTEM_ERROR
} os_monotonic_nanoseconds_tag;
typedef struct os_monotonic_nanoseconds_result {
    os_monotonic_nanoseconds_tag tag;
    i32 native_error;
} os_monotonic_nanoseconds_result;

typedef enum os_memory_reserve_tag {
    OS_MEMORY_RESERVE_OK, OS_MEMORY_RESERVE_INVALID_ARGUMENT, OS_MEMORY_RESERVE_OUT_OF_MEMORY, OS_MEMORY_RESERVE_SYSTEM_ERROR
} os_memory_reserve_tag;
typedef struct os_memory_reserve_result {
    os_memory_reserve_tag tag;
    i32 native_error;
} os_memory_reserve_result;

typedef enum os_memory_commit_tag {
    OS_MEMORY_COMMIT_OK, OS_MEMORY_COMMIT_INVALID_ARGUMENT, OS_MEMORY_COMMIT_SYSTEM_ERROR
} os_memory_commit_tag;
typedef struct os_memory_commit_result {
    os_memory_commit_tag tag;
    i32 native_error;
} os_memory_commit_result;

typedef enum os_memory_decommit_tag {
    OS_MEMORY_DECOMMIT_OK, OS_MEMORY_DECOMMIT_INVALID_ARGUMENT, OS_MEMORY_DECOMMIT_SYSTEM_ERROR
} os_memory_decommit_tag;
typedef struct os_memory_decommit_result {
    os_memory_decommit_tag tag;
    i32 native_error;
} os_memory_decommit_result;

typedef enum os_memory_free_tag {
    OS_MEMORY_FREE_OK, OS_MEMORY_FREE_INVALID_ARGUMENT, OS_MEMORY_FREE_SYSTEM_ERROR
} os_memory_free_tag;
typedef struct os_memory_free_result {
    os_memory_free_tag tag;
    i32 native_error;
} os_memory_free_result;

typedef enum thread_start_tag {
    THREAD_START_OK, THREAD_START_INVALID_ARGUMENT, THREAD_START_OUT_OF_MEMORY, THREAD_START_SYSTEM_ERROR
} thread_start_tag;
typedef struct thread_start_result {
    thread_start_tag tag;
    i32 native_error;
} thread_start_result;

typedef enum thread_join_tag {
    THREAD_JOIN_OK, THREAD_JOIN_INVALID_ARGUMENT, THREAD_JOIN_SYSTEM_ERROR
} thread_join_tag;
typedef struct thread_join_result {
    thread_join_tag tag;
    i32 native_error;
} thread_join_result;

typedef enum os_cpu_info_tag {
    OS_CPU_INFO_OK, OS_CPU_INFO_INVALID_ARGUMENT, OS_CPU_INFO_OUT_OF_MEMORY, OS_CPU_INFO_UNAVAILABLE, OS_CPU_INFO_SYSTEM_ERROR
} os_cpu_info_tag;
typedef struct os_cpu_info_result {
    os_cpu_info_tag tag;
    i32 native_error;
} os_cpu_info_result;

typedef enum os_event_create_tag {
    OS_EVENT_CREATE_OK, OS_EVENT_CREATE_INVALID_ARGUMENT, OS_EVENT_CREATE_SYSTEM_ERROR
} os_event_create_tag;
typedef struct os_event_create_result {
    os_event_create_tag tag;
    i32 native_error;
} os_event_create_result;

typedef enum os_event_signal_tag {
    OS_EVENT_SIGNAL_OK, OS_EVENT_SIGNAL_IO_ERROR, OS_EVENT_SIGNAL_SYSTEM_ERROR
} os_event_signal_tag;
typedef struct os_event_signal_result {
    os_event_signal_tag tag;
    i32 native_error;
} os_event_signal_result;

typedef enum os_event_wait_tag {
    OS_EVENT_WAIT_OK, OS_EVENT_WAIT_INVALID_ARGUMENT, OS_EVENT_WAIT_IO_ERROR, OS_EVENT_WAIT_SYSTEM_ERROR
} os_event_wait_tag;
typedef struct os_event_wait_result {
    os_event_wait_tag tag;
    i32 native_error;
} os_event_wait_result;

typedef enum os_event_consume_tag {
    OS_EVENT_CONSUME_OK, OS_EVENT_CONSUME_IO_ERROR, OS_EVENT_CONSUME_SYSTEM_ERROR
} os_event_consume_tag;
typedef struct os_event_consume_result {
    os_event_consume_tag tag;
    i32 native_error;
} os_event_consume_result;

typedef enum io_create_tag {
    IO_CREATE_OK, IO_CREATE_INVALID_ARGUMENT, IO_CREATE_OUT_OF_MEMORY, IO_CREATE_SYSTEM_ERROR
} io_create_tag;
typedef struct io_create_result {
    io_create_tag tag;
    i32 native_error;
} io_create_result;

typedef enum io_destroy_tag {
    IO_DESTROY_OK, IO_DESTROY_BUSY
} io_destroy_tag;
typedef struct io_destroy_result {
    io_destroy_tag tag;
} io_destroy_result;

typedef enum io_wait_tag {
    IO_WAIT_OK, IO_WAIT_INVALID_ARGUMENT, IO_WAIT_IO_ERROR, IO_WAIT_SYSTEM_ERROR
} io_wait_tag;
typedef struct io_wait_result {
    io_wait_tag tag;
    i32 native_error;
} io_wait_result;

typedef enum io_poll_tag {
    IO_POLL_OK, IO_POLL_INVALID_ARGUMENT, IO_POLL_EMPTY, IO_POLL_IO_ERROR, IO_POLL_SYSTEM_ERROR
} io_poll_tag;
typedef struct io_poll_result {
    io_poll_tag tag;
    i32 native_error;
} io_poll_result;

typedef enum io_event_wait_tag {
    IO_EVENT_WAIT_OK, IO_EVENT_WAIT_INVALID_ARGUMENT, IO_EVENT_WAIT_NO_SPACE
} io_event_wait_tag;
typedef struct io_event_wait_result {
    io_event_wait_tag tag;
} io_event_wait_result;

typedef enum tcp_listen_tag {
    TCP_LISTEN_OK, TCP_LISTEN_INVALID_ARGUMENT, TCP_LISTEN_SYSTEM_ERROR
} tcp_listen_tag;
typedef struct tcp_listen_result {
    tcp_listen_tag tag;
    usize value; /* Only valid for TCP_LISTEN_OK. */
    i32 native_error;
} tcp_listen_result;

typedef enum tcp_accept_tag {
    TCP_ACCEPT_OK, TCP_ACCEPT_INVALID_ARGUMENT, TCP_ACCEPT_NO_SPACE
} tcp_accept_tag;
typedef struct tcp_accept_result {
    tcp_accept_tag tag;
} tcp_accept_result;

typedef enum tcp_receive_tag {
    TCP_RECEIVE_OK, TCP_RECEIVE_INVALID_ARGUMENT, TCP_RECEIVE_MESSAGE_TOO_LARGE, TCP_RECEIVE_NO_SPACE
} tcp_receive_tag;
typedef struct tcp_receive_result {
    tcp_receive_tag tag;
} tcp_receive_result;

typedef enum tcp_send_tag {
    TCP_SEND_OK, TCP_SEND_INVALID_ARGUMENT, TCP_SEND_MESSAGE_TOO_LARGE, TCP_SEND_NO_SPACE
} tcp_send_tag;
typedef struct tcp_send_result {
    tcp_send_tag tag;
} tcp_send_result;

typedef enum tcp_close_tag {
    TCP_CLOSE_OK, TCP_CLOSE_SYSTEM_ERROR
} tcp_close_tag;
typedef struct tcp_close_result {
    tcp_close_tag tag;
    i32 native_error;
} tcp_close_result;

