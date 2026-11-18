#define _GNU_SOURCE
#include "os.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <sys/socket.h>

int main(void)
{
    io_queue *queue;
    assert(IoCreate(2, &queue).tag == IO_CREATE_OK);
    io_completion polled = {.id = 123, .value = 456};
    assert(IoPoll(NULL, &polled).tag == IO_POLL_INVALID_ARGUMENT);
    assert(IoPoll(queue, NULL).tag == IO_POLL_INVALID_ARGUMENT);
    assert(IoPoll(queue, &polled).tag == IO_POLL_EMPTY);
    assert(polled.id == 123 && polled.value == 456);
    u64 start, end;
    assert(OsMonotonicNanoseconds(NULL).tag == OS_MONOTONIC_NANOSECONDS_INVALID_ARGUMENT);
    assert(OsMonotonicNanoseconds(&start).tag == OS_MONOTONIC_NANOSECONDS_OK);
    i32 event;
    assert(OsEventCreate(NULL).tag == OS_EVENT_CREATE_INVALID_ARGUMENT);
    os_event_signal_result invalid_event = OsEventSignal(-1);
    assert(invalid_event.tag == OS_EVENT_SIGNAL_SYSTEM_ERROR && invalid_event.native_error == EBADF);
    assert(OsEventWait(-1).tag == OS_EVENT_WAIT_INVALID_ARGUMENT);
    assert(OsEventCreate(&event).tag == OS_EVENT_CREATE_OK);
    assert(OsEventConsume(event).tag == OS_EVENT_CONSUME_OK);
    assert(OsEventSignal(event).tag == OS_EVENT_SIGNAL_OK);
    assert(OsEventSignal(event).tag == OS_EVENT_SIGNAL_OK);
    assert(IoEventWait(queue, event, 99).tag == IO_EVENT_WAIT_OK);
    io_completion wake;
    assert(IoWait(queue, &wake).tag == IO_WAIT_OK && wake.id == 99 && wake.tag == IO_COMPLETION_OK && wake.value > 0);
    assert(OsEventConsume(event).tag == OS_EVENT_CONSUME_OK);
    assert(OsEventConsume(event).tag == OS_EVENT_CONSUME_OK);
    assert(OsEventSignal(event).tag == OS_EVENT_SIGNAL_OK);
    assert(OsEventWait(event).tag == OS_EVENT_WAIT_OK);
    OsEventClose(event);
    assert(OsMonotonicNanoseconds(&end).tag == OS_MONOTONIC_NANOSECONDS_OK && end >= start);
    /* socketpair and setsockopt are external API boundaries. */
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    int send_size = 4096;
    assert(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF,
                      &send_size, sizeof(send_size)) == 0);

    static u8 storage[65536];
    byte_slice buffer = {storage, sizeof(storage)};
    /* A pending receive with no data must return immediately. Then polling
     * alone must submit and complete both sides without any blocking wait. */
    assert(TcpReceive(queue, sockets[1], (byte_slice){buffer.ptr, 1}, 90).tag == TCP_RECEIVE_OK);
    assert(IoPoll(queue, &polled).tag == IO_POLL_EMPTY);
    assert(polled.id == 123 && polled.value == 456);
    u8 input = 42;
    assert(TcpSend(queue, sockets[0], (const_byte_slice){&input, 1}, 91).tag == TCP_SEND_OK);
    u32 poll_seen = 0;
    while (poll_seen != 3) {
        io_poll_result ready = IoPoll(queue, &polled);
        assert(ready.tag == IO_POLL_OK || ready.tag == IO_POLL_EMPTY);
        if (ready.tag == IO_POLL_EMPTY) continue;
        assert(polled.tag == IO_COMPLETION_OK && polled.value == 1 && (polled.id == 90 || polled.id == 91));
        u32 bit = (u32)1 << (polled.id - 90);
        assert((poll_seen & bit) == 0);
        poll_seen |= bit;
    }
    assert(buffer.ptr[0] == input);
    assert(IoPoll(queue, &polled).tag == IO_POLL_EMPTY);
    assert(TcpSend(queue, sockets[0], (const_byte_slice){NULL, 1}, 1).tag == TCP_SEND_INVALID_ARGUMENT);
    assert(TcpReceive(queue, sockets[1],
                      (byte_slice){buffer.ptr, (usize)INT_MAX + 1}, 1).tag == TCP_RECEIVE_MESSAGE_TOO_LARGE);
    assert(TcpSend(queue, sockets[0], (const_byte_slice){0}, 11).tag == TCP_SEND_OK);
    assert(TcpSend(queue, sockets[0], (const_byte_slice){0}, 12).tag == TCP_SEND_OK);
    assert(TcpSend(queue, sockets[0], (const_byte_slice){0}, 13).tag == TCP_SEND_NO_SPACE);
    assert(IoDestroy(queue).tag == IO_DESTROY_BUSY);
    io_completion completion;
    u64 seen = 0;
    for (usize index = 0; index < 2; ++index) {
        assert(IoWait(queue, &completion).tag == IO_WAIT_OK);
        assert(completion.tag == IO_COMPLETION_OK && completion.value == 0);
        assert(completion.id == 11 || completion.id == 12);
        seen |= (u64)1 << (completion.id - 11);
    }
    assert(seen == 3);

    /* Force a partial send and verify the reported count is usable as a slice. */
    assert(TcpSend(queue, sockets[0],
                   (const_byte_slice){buffer.ptr, buffer.len}, 21).tag == TCP_SEND_OK);
    assert(IoWait(queue, &completion).tag == IO_WAIT_OK);
    assert(completion.id == 21);
    assert(completion.tag == IO_COMPLETION_OK && completion.value > 0 && (usize)completion.value < buffer.len);
    usize sent = (usize)completion.value;
    assert(TcpReceive(queue, sockets[1], buffer, 22).tag == TCP_RECEIVE_OK);
    assert(IoWait(queue, &completion).tag == IO_WAIT_OK);
    assert(completion.id == 22 && completion.tag == IO_COMPLETION_OK && (usize)completion.value == sent);
    assert(TcpClose(sockets[0]).tag == TCP_CLOSE_OK);
    assert(TcpReceive(queue, sockets[1], buffer, 23).tag == TCP_RECEIVE_OK);
    assert(IoWait(queue, &completion).tag == IO_WAIT_OK);
    assert(completion.id == 23 && completion.tag == IO_COMPLETION_OK && completion.value == 0);
    assert(TcpClose(sockets[1]).tag == TCP_CLOSE_OK);
    assert(TcpReceive(queue, INT_MAX, buffer, 24).tag == TCP_RECEIVE_OK);
    assert(IoWait(queue, &completion).tag == IO_WAIT_OK);
    assert(completion.id == 24 && completion.tag == IO_COMPLETION_BAD_HANDLE);
    assert(IoDestroy(queue).tag == IO_DESTROY_OK);
    TerminalWrite(StringSlice("PASS: queue limits, slice validation, short send, EOF\n"));
    return 0;
}
