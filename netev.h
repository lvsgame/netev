#ifndef __NETEV_H__ 
#define __NETEV_H__

#include <stdint.h>

#define NETEV_ERR_NOBUF
#define NETEV_ERR_BLOCK
#define NETEV_ERR_CLOSED

#define NETEV_READ  1
#define NETEV_WRITE 2 

#define NETEV_OK            0 //正常
#define NETEV_ERR_CONNECT   1 //连接失败
#define NETEV_ERR_SOCKET    2
#define NETEV_ERR_MSG       3
#define NETEV_ERR_INTERNAL  4

typedef void (*netev_listencb) (int fd, int id);
typedef void (*netev_connectcb)(int fd, int id, void* data, int error);
typedef void (*netev_readcb)   (int fd, int id, void* data);
typedef void (*netev_writecb)  (int fd, int id, void* data);

struct netev;

struct netev* netev_create(int max, int block_size);
void netev_free(struct netev* self);

int netev_poll(struct netev* self, int timeout);
int netev_add_event(struct netev* self, int id, int mask, netev_readcb rcb, netev_writecb wcb, void* data);
int netev_del_event(struct netev* self, int id);
void* netev_read(struct netev* self, int id, int size);
int netev_write(struct netev* self, int id, const void* data, int size);
void netev_dropread(struct netev* self, int id);
int netev_listen(struct netev* self, uint32_t addr, uint16_t port, netev_listencb cb);
int netev_connect(struct netev* self, uint32_t addr, uint16_t port, int block, netev_connectcb cb, void* data);
void netev_close_socket(struct netev* self, int id);
int netev_error(struct netev* self);

#endif
