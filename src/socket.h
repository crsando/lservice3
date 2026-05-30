#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uv.h"

#define SERVICE_DEFAULT_BACKLOG 128

typedef struct {
    struct sockaddr_in addr;
    uv_tcp_t server;

    int on_conn_ref; // reference to lua function
} service_socket_t;


static inline int service_socket_bind(service_socket_t * socket, const char * ip, unsigned int port) {
    uv_ip4_addr(ip, port, &(socket->addr));
}

static inline int service_socket_listen(service_socket_t * socket) { 
    uv_listen(&(socket->server), SERVICE_DEFAULT_BACKLOG, on_new_connection);
}

static inline void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    buf->base = malloc(suggested_size);
    buf->len = suggested_size;
}

static inline void on_read(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf) {
    if (nread > 0) {
        printf("received %zd bytes: %.*s\n", nread, (int)nread, buf->base);

        // 读取到字节后，我们需要通知回service，从service继续coroutine


        // 注意：这里不能 free(buf->base)
        // 因为写操作还没完成，等 on_write 里释放
        return;
    }
    if (nread < 0) {
        if (nread != UV_EOF) {
            fprintf(stderr, "read error: %s\n", uv_strerror((int)nread));
        }
        uv_close((uv_handle_t *)client, on_client_closed);
    }
    if (buf->base) {
        free(buf->base);
    }
}

static inline void on_client_closed(uv_handle_t *handle) {
    free(handle);
}

static inline void on_new_connection(uv_stream_t *server, int status) {
    if (status < 0) {
        fprintf(stderr, "new connection error: %s\n", uv_strerror(status));
        return;
    }
    uv_tcp_t * client = malloc(sizeof(uv_tcp_t));
    uv_tcp_init(server->loop, client);
    if (uv_accept(server, (uv_stream_t *)client) == 0) {
        printf("client connected\n");
        uv_read_start(
            (uv_stream_t *)client, // 我们在这个client.data 里绑定lua中的upvalue
            alloc_buffer,
            on_read
        );
    } else {
        uv_close((uv_handle_t *)client, on_client_closed);
    }
}


#undef SERVICE_DEFAULT_BACKLOG