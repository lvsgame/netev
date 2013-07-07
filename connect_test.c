#include "netev.h"
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#pragma pack(1)
struct msg_header {
    uint16_t size;
};

struct msg {
    uint16_t size;
    char text[1024];
};
#pragma pack()

static struct netev* ne = NULL;

void
readcb(int fd, int id, void* data) {
    int error = NETEV_OK;
    for (;;) {
        struct msg_header* h = netev_read(ne, id, sizeof(struct msg_header));
        if (h == NULL) {
            error = netev_error(ne);
            if (error == NETEV_OK) {
                goto out;
            } else {
                goto err_out;
            }
        }
        void* msg = netev_read(ne, id, h->size);
        if (msg == NULL) {
            error = netev_error(ne);
            if (error == NETEV_OK) {
                goto out;
            } else {
                goto err_out;
            }
        }
        printf("client %d read msg size=%d\n", id, h->size);
        netev_dropread(ne, id);
    }
out:
    return;
err_out:
    printf("client %d read occur error %d\n", id, error);
    netev_close_socket(ne, id);
    return;
}

void 
writecb(int fd, int id, void* data) {
    struct msg m;
    m.size = sizeof(m.text);
    int i;
    int c = 0;
    for (i=0; i<sizeof(m.text); ++i) {
        m.text[i] = c+'0';
        if (c >= 9)
            c = 0;
    }
    int wsize = 0;
    while (wsize < sizeof(m)) {
        int nbyte = send(fd, (void*)&m + wsize, sizeof(m) - wsize, 0);
        if (nbyte >= 0) {
            wsize += nbyte;
            continue;
        }
        if (nbyte == -1) {
            if (errno != EAGAIN &&
                errno != EWOULDBLOCK) {
                printf("send to server error %d, %s\n", errno, strerror(errno));
                netev_close_socket(ne, id);
                break;
            }
        }
    }
    printf("send to server msg size=%lu\n", sizeof(m));
}

void 
_connectcb(int fd, int id, void* data, int error) {
    struct sockaddr_in remote_addr;
    socklen_t len = sizeof(remote_addr);
    getpeername(fd, (struct sockaddr*)&remote_addr, &len);

    if (error == 0)
        printf("connect ok %d,%d, %s:%u\n", fd, id, 
            inet_ntoa(remote_addr.sin_addr), 
            ntohs(remote_addr.sin_port));
    else
        printf("connect failed %u, %s\n", error, strerror(error));

    if (error == 0)
        netev_add_event(ne, id, NETEV_READ|NETEV_WRITE, readcb, writecb, NULL);
}

int 
main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("usage: %s ip:port\n", argv[0]);
        return -1;
    }

    char ip_port[24] = {0};
    strncpy(ip_port, argv[1], sizeof(ip_port));

    uint32_t addr = INADDR_ANY;
    uint16_t port = 0;

    char* tmp = strchr(ip_port, ':');
    if (tmp == NULL) {
        port = strtol(ip_port, NULL, 10);
    } else {
        port = strtol(tmp+1, NULL, 10);
        *tmp = '\0';
        addr = inet_addr(ip_port);
    }

    int max = 10;
    if (argc > 2)
        max = strtol(argv[2], NULL, 10);

    int buf_size = 64;
    if (argc > 3)
        buf_size = strtol(argv[3], NULL, 10);


    ne = netev_create(max, buf_size*1024);
  
    printf("connect to %s\n", argv[1]);

    int r = netev_connect(ne, addr, port, 0, _connectcb, NULL);
    if (r != 0) {
        return -1; 
    } 

    for (;;) {
        netev_poll(ne, 1);
        usleep(1000000);
    }
    netev_free(ne);
    return 0;
}
