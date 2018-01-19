PORT=4406
BACKEND_PORT=8081
LOGPORT=28122
PREFIX=/home/$(shell whoami)
LIBDIR=$(PREFIX)/lib

CFLAGS=-Wall -g -I $(HOME)/include -I/usr/include/httpd -I/usr/include/apr-1 `pkg-config --cflags --libs openssl apr-1 ncurses` -lapr-1 -laprutil-1 -fPIC -L$(HOME)/lib -lparsegraph_user -lparsegraph_List -lparsegraph_environment

all: src/test-ring.sh src/test-connection.sh
	cd src && ./test-ring.sh
	cd src && ./test-connection.sh $(PORT)
	cd servermod && $(MAKE)
	cd environment_ws && $(MAKE)
	$(MAKE) marla
.PHONY: all

src/test-ring.sh: src/test_ring src/test_small_ring src/test_ring_putback src/test_ring_po2

src/test-connection.sh: src/test_connection src/test_websocket src/test_chunks src/test_backend

servermod/libservermod.so:
	cd servermod && $(MAKE)

environment_ws/libenvironment_ws.so:
	cd environment_ws && $(MAKE)

BASE_OBJECTS=src/ring.o src/connection.o src/request.o src/client.o src/log.o src/backend.o src/hooks.o src/chunks.o src/ssl.o src/cleartext.o src/terminal.o src/server.o

libmarla.so: $(BASE_OBJECTS) src/marla.h
	$(CC) $(CFLAGS) -o$@ -shared -lpthread $(BASE_OBJECTS)

marla: src/main.c libmarla.so src/marla.h Makefile
	$(CC) src/main.c -o$@ -lpthread  -L. -lmarla $(CFLAGS)

certificate.pem key.pem:
	openssl req -newkey rsa:2048 -nodes -keyout key.pem -x509 -days 365 -out certificate.pem

kill: marla.tmux
	tmux -S marla.tmux kill-server
.PHONY: kill

run: marla certificate.pem key.pem servermod/libservermod.so environment_ws/libenvironment_ws.so
	tmux -S marla.tmux new-s -d ./marla $(PORT) $(BACKEND_PORT) $(LOGPORT) servermod/libservermod.so?module_servermod_init environment_ws/libenvironment_ws.so?module_environment_ws_init
.PHONY: run

tmux:
	tmux -S marla.tmux att
.PHONY: tmux

check: certificate.pem src/test_ring src/test_small_ring src/test_ring_putback src/test_connection src/test_websocket src/test_chunks src/test_backend
	cd src || exit; \
	for i in seq 3; do \
	echo Running connecting tests; \
	echo Running low buffer-size tests; \
	./test_low.sh $(PORT) TEST || exit; \
	./test_low_lf.sh $(PORT) TEST || exit; \
	done
.PHONY: check

src/test_ring: src/test_ring.c src/ring.o
	$(CC) $(CFLAGS) -g $^ -o$@

src/test_ring_po2: src/test_ring_po2.c src/ring.o
	$(CC) $(CFLAGS) -g $^ -o$@

src/test_small_ring: src/test_small_ring.c src/ring.o
	$(CC) $(CFLAGS) -g $^ -o$@

src/test_ring_putback: src/test_ring_putback.c src/ring.o
	$(CC) $(CFLAGS) -g $^ -o$@

src/test_connection: src/test_connection.c $(BASE_OBJECTS) src/marla.h Makefile
	$(CC) $(CFLAGS) -g src/test_connection.c $(BASE_OBJECTS) -o$@

src/test_websocket: src/test_websocket.c $(BASE_OBJECTS) src/marla.h Makefile
	$(CC) $(CFLAGS) -g src/test_websocket.c $(BASE_OBJECTS) -o$@

src/test_chunks: src/test_chunks.c $(BASE_OBJECTS) src/marla.h Makefile
	$(CC) $(CFLAGS) -g src/test_chunks.c $(BASE_OBJECTS) -o$@

src/test_backend: src/test_backend.c $(BASE_OBJECTS) src/marla.h Makefile
	$(CC) $(CFLAGS) -g src/test_backend.c $(BASE_OBJECTS) -o$@

clean:
	rm -f libmarla.so marla *.o src/*.o marla.a
	cd servermod && $(MAKE) clean
	cd environment_ws && $(MAKE) clean
	rm -f src/test_connection src/test_websocket src/test_ring src/test_ring_putback src/test_small_ring test-client src/test_backend
.PHONY: clean

clean-certificate: | certificate.pem key.pem
	rm certificate.pem key.pem
.PHONY: clean-certificate

install: libmarla.so
	cp $^ $(LIBDIR)
.PHONY: install
