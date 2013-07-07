#include "netev.h"
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

static struct netev* ne = NULL;

void
readcb(int fd, int id, void* data) {
    char buf[1024];
    int nbyte = recv(fd, buf, sizeof(buf)-1, MSG_DONTWAIT);
    if (nbyte > 0) {
        buf[nbyte] = '\0';
        printf("recv: %s", buf);
        send(fd, buf, nbyte, 0);
        return;
    }
    if (nbyte == 0) {
        printf("remote client closed %d, errno %d\n", id, errno);
        netev_close_socket(ne, id);
        return;
    }
    if (nbyte == -1 ) {
        if (errno == EAGAIN ||
            errno == EWOULDBLOCK)
            return;
        else {
            printf("remote client closed %d, errno %d\n", id, errno);
            return;
        }
    }
    return;
}
void 
listencb(int fd, int id) {
    struct sockaddr_in remote_addr;
    socklen_t len = sizeof(remote_addr);
    getpeername(fd, (struct sockaddr*)&remote_addr, &len);
    printf("new client %d,%d, %s:%u\n", fd, id, 
        inet_ntoa(remote_addr.sin_addr), 
        ntohs(remote_addr.sin_port));

    netev_add_event(ne, id, NETEV_READ, readcb, NULL, NULL);
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
    ne = netev_create(10, 64*1024);
    
    int r = netev_listen(ne, addr, port, listencb);
    if (r != 0) {
        return -1; 
    }
    printf("listen on %s\n", argv[1]);

    for (;;) {
        netev_poll(ne, 1);
        usleep(1000000);
    }
    netev_free(ne);
    return 0;
}
