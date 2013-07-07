.PHONY: all clean

CFLAGS = -g -Wall
SHARED = -fPIC -shared
#ALL = libnetev.a libnetev.so connect_test listen_test server client
ALL = libnetev.a connect_test listen_test server client
all: $(ALL)

OBJS = netev.o netbuf.o

libnetev.so: netev.c netev.h netbuf.c netbuf.h
	rm -f $@
	gcc $(CFLAGS) $(SHARED) $^ -o $@

$(OBJS): %.o: %.c
	gcc -c $(CFLAGS) $< -o $@

libnetev.a: $(OBJS)
	rm -f $@
	ar crus $@ $(OBJS)

listen_test: listen_test.c
	gcc $(CFLAGS) $^ -o $@ -lnetev -L.

connect_test: connect_test.c
	gcc $(CFLAGS) $^ -o $@ -lnetev -L.

server: server.c
	gcc $(CFLAGS) $^ -o $@ -lnetev -L. -lrt

client: client.c
	gcc $(CFLAGS) $^ -o $@ -lnetev -L.

zlib_test: zlib_test.c
	gcc $(CFLAGS) $^ -o $@ -lz -L../zlib-1.2.8 -lrt

clean:
	rm -f $(ALL) *.o zlib_test
