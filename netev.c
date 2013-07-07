#include "netev.h"
#include "netbuf.h"
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>

#define STATUS_INVALID     0
#define STATUS_SUSPEND     1
#define STATUS_CONNECTING  2
#define STATUS_CONNECTED   3
#define STATUS_OPENED      STATUS_SUSPEND

#define LISTEN_BACKLOG 500
#define LISTEN_SOCKET (void*)((intptr_t)~0)

struct socket {
    int fd;
    int status;
    struct netbuf_block* rbuf_b;

    netev_readcb rcb;
    netev_writecb wcb;
    void* data;
};

struct netev {
    int epoll_fd;

    int listen_fd;
    netev_listencb listen_cb;

    int max;
    struct epoll_event* events;

    struct socket* sockets;
    struct socket* free_socket;

    struct netbuf* rbuf;

    int error;
};

static inline int
_add_event(struct netev* self, struct socket* s, int events) {
    if (events == 0)
        return -1;

    int op = (s->rcb == NULL && s->wcb == NULL) ? 
        EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    
    struct epoll_event ev;
    ev.events = events;
    ev.data.ptr = s;
    return epoll_ctl(self->epoll_fd, op, s->fd, &ev);
}

static inline int
_del_event(struct netev* self, struct socket* s) {
    if (s->rcb == NULL && s->wcb == NULL)
        return -1;
    struct epoll_event ev;
    ev.events = 0;
    ev.data.ptr = s;
    return epoll_ctl(self->epoll_fd, EPOLL_CTL_DEL, s->fd, &ev);
}

static inline int
_set_nonblocking(int fd) {
    int flag = fcntl(fd, F_GETFL, 0);
    if (flag == -1)
        return -1;
    return fcntl(fd, F_SETFL, flag | O_NONBLOCK);
}

static inline int
_set_closeonexec(int fd) {
    int flag = fcntl(fd, F_GETFL, 0);
    if (flag == -1)
        return -1;
    return fcntl(fd, F_SETFL, flag | FD_CLOEXEC);
}

static inline int
_set_reuseaddr(int fd) {
    int reuse = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
}

static struct socket*
_alloc_sockets(int max) {
    int i;
    struct socket* s = malloc(max * sizeof(struct socket));
    for (i=0; i<max; ++i) {
        s[i].fd = i+1;
        s[i].status = STATUS_INVALID;
        s[i].rbuf_b = NULL;
        s[i].rcb = NULL;
        s[i].wcb = NULL;
        s[i].data = NULL;
    }
    s[max-1].fd = -1;
    return s;
}

static inline struct socket*
_create_socket(struct netev* self, int fd) {
    if (self->free_socket == NULL)
        return NULL;

    struct socket* s = self->free_socket;
    if (s->fd >= 0)
        self->free_socket = &self->sockets[s->fd];
    else
        self->free_socket = NULL;
    
    s->fd = fd; 
    s->status = STATUS_SUSPEND;
    s->rbuf_b = netbuf_alloc_block(self->rbuf, s-self->sockets);
    return s;
}

static inline void
_close_socket(struct netev* self, struct socket* s) {
    if (s->status == STATUS_INVALID)
        return;

    _del_event(self, s);
    close(s->fd);
    
    s->fd = self->free_socket ? self->free_socket - self->sockets : -1;
    s->status = STATUS_INVALID;
    
    netbuf_free_block(self->rbuf, s->rbuf_b);
    s->rbuf_b = NULL;
    
    s->rcb = NULL;
    s->wcb = NULL;
    s->data = NULL;

    self->free_socket = s;
}

static inline struct socket*
_get_socket(struct netev* self, int id) {
    assert(id >= 0 && id < self->max);
    return &self->sockets[id];
}

void 
netev_close_socket(struct netev* self, int id) {
    struct socket* s = _get_socket(self, id);
    if (s) {
        _close_socket(self, s);
    }
}

int
netev_add_event(struct netev* self, int id, int mask, netev_readcb rcb, netev_writecb wcb, void* data) {
    struct socket* s = _get_socket(self, id);
    if (s == NULL)
        return -1;
    uint32_t events = 0;
    if ((mask & NETEV_READ) && rcb) {
        events |= EPOLLIN;
    }
    if ((mask & NETEV_WRITE) && wcb) {
        events |= EPOLLOUT;
    }
    if (events == 0)
        return -1;
    int r = _add_event(self, s, events);
    if (r != -1) { 
        s->rcb = (mask & NETEV_READ)  ? rcb : NULL;
        s->wcb = (mask & NETEV_WRITE) ? wcb : NULL;
        s->data = data;
    }
    return r;
}

int
netev_del_event(struct netev* self, int id) {
    struct socket* s = _get_socket(self, id);
    if (s == NULL)
        return -1;
    int r = _del_event(self, s);
    if (r == 0) {
        s->rcb = NULL;
        s->wcb = NULL;
    }
    return r;
}

struct netev*
netev_create(int max, int block_size) {
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    if (max == 0 || block_size == 0)
        return NULL;

    int epoll_fd = epoll_create(max+1);
    if (epoll_fd == -1) {
        return NULL;
    }
    if (_set_closeonexec(epoll_fd) == -1) {
        return NULL;
    }
    struct netev* ne = malloc(sizeof(struct netev));
    ne->epoll_fd = epoll_fd;
    ne->listen_fd = -1;
    ne->listen_cb = NULL;
    ne->max = max;
    ne->events = malloc(max * sizeof(struct epoll_event));
    ne->sockets = _alloc_sockets(max);
    ne->free_socket = &ne->sockets[0];
    ne->rbuf = netbuf_create(max, block_size);
    ne->error = NETEV_OK;
    return ne;
}

void
netev_free(struct netev* self) {
    if (self == NULL)
        return;

    int i;
    for (i=0; i<self->max; ++i) {
        struct socket* s = &self->sockets[i];
        if (s->status >= STATUS_OPENED) {
            _close_socket(self, s);
        }
    }
    free(self->sockets);
    free(self->events);
    netbuf_free(self->rbuf);

    if (self->listen_fd >= 0) {
        close(self->listen_fd);
    }
    close(self->epoll_fd);
    free(self);
}

void*
netev_read(struct netev* self, int id, int size) {
    self->error = NETEV_OK;
        
    struct socket* s = _get_socket(self, id);
    if (s == NULL) {
        self->error = NETEV_ERR_INTERNAL;
        return NULL;
    }

    if (size <= 0) {
        _close_socket(self, s);
        self->error = NETEV_ERR_MSG;
        return NULL;
    }

    struct netbuf_block* rbuf_b = s->rbuf_b;
    
    void* rptr = (void*)rbuf_b + sizeof(*rbuf_b) + rbuf_b->roffset;
 
    if (rbuf_b->woffset - rbuf_b->roffset >= size) {
        rbuf_b->roffset += size;
        return rptr; 
    }

    void* wptr = (void*)rbuf_b + sizeof(*rbuf_b) + rbuf_b->woffset;
    int space = rbuf_b->size - rbuf_b->woffset;
    if (space < size) {
        _close_socket(self, s);
        self->error = NETEV_ERR_MSG;
        return NULL; 
    }

    int nbyte = read(s->fd, wptr, space);
    if (nbyte > 0) {
        rbuf_b->woffset += nbyte;
        if (rbuf_b->woffset - rbuf_b->roffset >= size) {
            rbuf_b->roffset += size;
            return rptr;
        } else {
            rbuf_b->roffset = 0;
            return NULL;
        }
    } 
    if (nbyte == 0) { 
        _close_socket(self, s);
        self->error = NETEV_ERR_SOCKET;
        return NULL;
    } 
    if (errno == EAGAIN || 
        errno == EWOULDBLOCK) {
        rbuf_b->roffset = 0;
        return NULL;
    } else {
        _close_socket(self, s);
        self->error = NETEV_ERR_SOCKET;
        return NULL;
    }
    return NULL;
}

void
netev_dropread(struct netev* self, int id) {
    struct socket* s = _get_socket(self, id);
    if (s == NULL)
        return;
    
    struct netbuf_block* rbuf_b = s->rbuf_b;
    assert(rbuf_b->roffset >= 0);
    if (rbuf_b->roffset == 0)
        return;
    
    int size = rbuf_b->woffset - rbuf_b->roffset;
    if (size == 0) {
        rbuf_b->woffset = 0;
        rbuf_b->roffset = 0;
    } else if (size > 0) {
        void* buf = (void*)rbuf_b + sizeof(*rbuf_b);
        memmove(buf, buf + rbuf_b->roffset, size);
        rbuf_b->woffset = size;
        rbuf_b->roffset = 0;
    }
}

int
netev_write(struct netev* self, int id, const void* data, int size) {
    self->error = NETEV_OK;
    if (size <= 0) {
        return 0;
    }
    
    struct socket* s = _get_socket(self, id);
    if (s == NULL) {
        self->error = NETEV_ERR_INTERNAL;
        return -1;
    }

    int nbyte = write(s->fd, data, size);
    if (nbyte >= 0) {
        return nbyte; 
    }
    if (errno == EAGAIN ||
        errno == EWOULDBLOCK) {
        return 0;
    } else {
        _close_socket(self, s);
        self->error = NETEV_ERR_SOCKET;
        return -1;
    }
}

static inline int
_accept(struct netev* self) {
    struct sockaddr_in remote_addr;
    socklen_t len = sizeof(remote_addr);
    int fd = accept(self->listen_fd, (struct sockaddr*)&remote_addr, &len);
    if (fd == -1)
        return -1;
    struct socket* s = _create_socket(self, fd);
    if (s == NULL) {
        close(fd);
        return -1;
    }

    if (_set_nonblocking(fd) == -1) {
        _close_socket(self, s);
        return -1;
    }
    s->status = STATUS_CONNECTED;
    self->listen_cb(s->fd, s - self->sockets);
    return 0;
}

int
netev_listen(struct netev* self, uint32_t addr, uint16_t port, netev_listencb cb) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1)
        return -1;

    if (_set_nonblocking(fd) == -1 ||
        _set_closeonexec(fd) == -1 ||
        _set_reuseaddr(fd)   == -1) {
        close(fd);
        return -1;
    }
    
    struct sockaddr_in my_addr;
    memset(&my_addr, 0, sizeof(struct sockaddr_in));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(port);
    my_addr.sin_addr.s_addr = addr;
    if (bind(fd, (struct sockaddr*)&my_addr, sizeof(struct sockaddr)) == -1) {
        close(fd);
        return -1;
    }   

    if (listen(fd, LISTEN_BACKLOG) == -1) {
        close(fd);
        return -1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = LISTEN_SOCKET;
    if (epoll_ctl(self->epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        close(fd);
        return -1;
    }

    self->listen_fd = fd;
    self->listen_cb = cb;
    return 0;
}

static inline int
_onconnect(struct netev* self, struct socket* s) {
    if (s->status != STATUS_CONNECTING)
        return -1;
    int err;
    socklen_t errlen = sizeof(err);
    if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &errlen) == -1) {
        if (err == 0)
            err = errno != 0 ? errno : -1;
    }
    netev_connectcb cb = (netev_connectcb)s->wcb;
    if (err == 0) {
        s->status = STATUS_CONNECTED;
    }
    if (cb) {
        cb(s->fd, s - self->sockets, s->data, err);
    }
    if (err) {
        _close_socket(self, s);
        return -1;
    } else {
        return 0;
    }
}

int
netev_connect(struct netev* self, uint32_t addr, uint16_t port, int block, 
        netev_connectcb cb, void* data) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1)
        return -1;
 
    if (!block)
        if (_set_nonblocking(fd) == -1)
            return -1;

    int status;
    struct sockaddr_in my_addr;
    memset(&my_addr, 0, sizeof(struct sockaddr_in));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(port);
    my_addr.sin_addr.s_addr = addr;
    int r = connect(fd, (struct sockaddr*)&my_addr, sizeof(struct sockaddr));
    if (r == -1) {
        if (block || errno != EINPROGRESS) {
            close(fd);
            return -1;
        }
        status = STATUS_CONNECTING;
    } else {
        status = STATUS_CONNECTED;
    }

    if (block)
        if (_set_nonblocking(fd) == -1) // 仅connect阻塞
            return -1;

    struct socket* s = _create_socket(self, fd);
    if (s == NULL) {
        close(fd);
        return -1;
    }
   
    s->status = status;
    if (s->status == STATUS_CONNECTED) {
        cb(s->fd, s-self->sockets, s->data, 0);
    } else {
        if (_add_event(self, s, EPOLLIN|EPOLLOUT) == -1) {
            _close_socket(self, s);
            return -1;
        } 
        s->wcb = (netev_writecb)cb;
        s->data = data; 
    }
    return 0;
}

int
netev_poll(struct netev* self, int timeout) {
    int i;
    int nfd = epoll_wait(self->epoll_fd, self->events, 1000/*self->max*/, timeout); 
    for (i=0; i<nfd; ++i) {
        struct epoll_event* ev = &self->events[i];
        struct socket* s = ev->data.ptr;
        if (s == LISTEN_SOCKET) {
            _accept(self);
            continue;
        }
        if (s->status == STATUS_CONNECTING) {
            if (ev->events & EPOLLOUT) {
                if (_onconnect(self, s) == 0) {
                    if ((ev->events & EPOLLIN) &&
                        s->rcb) { // 可写并且可读
                        s->rcb(s->fd, s - self->sockets, s->data);
                    }
                }
            }
            continue;
        }
        if ((ev->events & EPOLLIN) &&
            s->rcb &&
            s->status == STATUS_CONNECTED) {
            s->rcb(s->fd, s - self->sockets, s->data);
        }
        if ((ev->events & EPOLLOUT) &&
            s->wcb &&
            s->status == STATUS_CONNECTED) {
            s->wcb(s->fd, s - self->sockets, s->data);
        }
    }
    return nfd;
}

int 
netev_error(struct netev* self) {
    return self->error;
}
