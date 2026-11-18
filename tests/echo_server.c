#include "os.h"
#include "slice.h"
#include <assert.h>

enum { CONNECTION_LIMIT = 128, BUFFER_SIZE = 4096 };
enum { OP_ACCEPT, OP_RECEIVE, OP_SEND, OP_COUNT };
enum { SOCKET_FREE = -1, SOCKET_ACCEPTING = -2 };
typedef struct connection { i32 socket; byte_slice buffer; const_byte_slice pending; } connection;
typedef struct connection_slice { connection *ptr; usize len; } connection_slice;

static void QueueAccept(io_queue *queue, i32 listener, connection_slice clients)
{
    SliceFor(clients, index) {
        if (clients.ptr[index].socket == SOCKET_ACCEPTING) return;
    }
    SliceFor(clients, index) {
        if (clients.ptr[index].socket == SOCKET_FREE) {
            assert(TcpAccept(queue, listener, index * OP_COUNT + OP_ACCEPT).tag == TCP_ACCEPT_OK);
            clients.ptr[index].socket = SOCKET_ACCEPTING;
            return;
        }
    }
}

static void HandleCompletion(io_queue *queue, connection_slice clients, io_completion completion)
{
    usize index = (usize)(completion.id / OP_COUNT);
    u32 operation = completion.id % OP_COUNT;
    assert(index < clients.len);
    connection *client = &clients.ptr[index];
    if (operation == OP_ACCEPT) {
        if (completion.tag != IO_COMPLETION_OK) {
            client->socket = SOCKET_FREE;
            assert(completion.tag == IO_COMPLETION_INTERRUPTED || completion.tag == IO_COMPLETION_WOULD_BLOCK ||
                completion.tag == IO_COMPLETION_ABORTED || completion.tag == IO_COMPLETION_PROTOCOL_ERROR);
            return;
        }
        client->socket = (i32)completion.value;
        assert(TcpReceive(queue, client->socket, client->buffer, index * OP_COUNT + OP_RECEIVE).tag == TCP_RECEIVE_OK);
        return;
    }
    if (completion.tag == IO_COMPLETION_INTERRUPTED || completion.tag == IO_COMPLETION_WOULD_BLOCK) {
        if (operation == OP_RECEIVE) {
            assert(TcpReceive(queue, client->socket, client->buffer, completion.id).tag == TCP_RECEIVE_OK);
        } else {
            assert(TcpSend(queue, client->socket, client->pending, completion.id).tag == TCP_SEND_OK);
        }
        return;
    }
    if (completion.tag != IO_COMPLETION_OK || completion.value == 0) {
        TcpClose(client->socket);
        client->socket = SOCKET_FREE;
        client->pending = (const_byte_slice){0};
        return;
    }
    if (operation == OP_RECEIVE) {
        assert(completion.value <= client->buffer.len);
        client->pending = (const_byte_slice){client->buffer.ptr, completion.value};
    } else {
        assert(completion.value <= client->pending.len);
        assert(ConstByteSliceSubslice(client->pending, completion.value, client->pending.len - completion.value,
            &client->pending).tag == CONST_BYTE_SLICE_SUBSLICE_OK);
    }
    if (client->pending.len != 0) {
        assert(TcpSend(queue, client->socket, client->pending, index * OP_COUNT + OP_SEND).tag == TCP_SEND_OK);
    } else {
        assert(TcpReceive(queue, client->socket, client->buffer, index * OP_COUNT + OP_RECEIVE).tag == TCP_RECEIVE_OK);
    }
}

int main(void)
{
    static connection client_storage[CONNECTION_LIMIT];
    static u8 buffer_storage[CONNECTION_LIMIT * BUFFER_SIZE];
    connection_slice clients = {client_storage, CONNECTION_LIMIT};
    byte_slice buffers = {buffer_storage, sizeof(buffer_storage)};
    SliceFor(clients, index) {
        clients.ptr[index].socket = SOCKET_FREE;
        assert(ByteSliceSubslice(buffers, index * BUFFER_SIZE, BUFFER_SIZE,
            &clients.ptr[index].buffer).tag == BYTE_SLICE_SUBSLICE_OK);
    }
    io_queue *queue;
    assert(IoCreate(CONNECTION_LIMIT, &queue).tag == IO_CREATE_OK);
    tcp_listen_result listener = TcpListen(0x7f000001, 8080, CONNECTION_LIMIT);
    assert(listener.tag == TCP_LISTEN_OK);
    TerminalWrite(StringSlice("Echo server listening on 127.0.0.1:8080\n"));
    for (;;) {
        QueueAccept(queue, (i32)listener.value, clients);
        io_completion completion;
        assert(IoWait(queue, &completion).tag == IO_WAIT_OK);
        HandleCompletion(queue, clients, completion);
    }
}
