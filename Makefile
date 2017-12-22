PORT=4479
BACKEND_PORT=8081
PREFIX=/home/$(shell whoami)
LIBDIR=$(PREFIX)/lib

CXXFLAGS=-g -I $(HOME)/include -I/usr/include/httpd -I/usr/include/apr-1 `pkg-config --cflags --libs openssl apr-1 ncurses` -lapr-1 -laprutil-1 -fPIC -L$(HOME)/lib -lparsegraph_user -lparsegraph_List -lparsegraph_environment

all: rainback
	cd servermod && $(MAKE)
	cd environment_ws && $(MAKE)
.PHONY: all

servermod/libservermod.so:
	cd servermod && $(MAKE)

environment_ws/libenvironment_ws.so:
	cd environment_ws && $(MAKE)

librainback.so: src/ring.c src/connection.c src/client.c src/backend.c src/websocket_handler.c src/hooks.c src/default_request_handler.c src/ssl.c src/cleartext.c src/terminal.c src/server.c | src/rainback.h Makefile
	$(CC) $(CXXFLAGS) -shared -o$@ -g $^

rainback: src/main.c librainback.so | src/rainback.h
	$(CC) $(CXXFLAGS) src/main.c -o$@ -lpthread -L. -lrainback

certificate.pem key.pem:
	openssl req -newkey rsa:2048 -nodes -keyout key.pem -x509 -days 365 -out certificate.pem

kill: rainback.tmux
	tmux -S rainback.tmux kill-server
.PHONY: kill

run: rainback certificate.pem key.pem servermod/libservermod.so environment_ws/libenvironment_ws.so
	tmux -S rainback.tmux new-s -d ./rainback $(PORT) $(BACKEND_PORT) servermod/libservermod.so?module_servermod_init environment_ws/libenvironment_ws.so?module_environment_ws_init
.PHONY: run

tmux:
	tmux -S rainback.tmux att
.PHONY: tmux

check: certificate.pem tests/run-tests tests/test_ring tests/test_small_ring tests/test_ring_putback tests/test_connection tests/test_websocket
	cd tests || exit; \
	./test-ring.sh || exit; \
	./test_connection $(PORT) || exit; \
	./test_websocket $(PORT) || exit; \
	for i in seq 3; do \
	./run-tests.sh $(PORT) || exit; \
	./test_low.sh $(PORT) TEST || exit; \
	./test_low_lf.sh $(PORT) TEST || exit; \
	done
.PHONY: check

tests/test_connection: tests/test_connection.c
	$(CC) $(CXXFLAGS) -g $^ -o$@ -L. -lrainback

tests/test_ring: tests/test_ring.c
	$(CC) $(CXXFLAGS) -g $^ -o$@ -L. -lrainback

tests/test_small_ring: tests/test_small_ring.c
	$(CC) $(CXXFLAGS) -g $^ -o$@ -L. -lrainback

tests/test_ring_putback: tests/test_ring_putback.c
	$(CC) $(CXXFLAGS) -g $^ -o$@ -L. -lrainback

tests/run-tests: tests/run-tests.c | src/rainback.h librainback.so
	$(CC) $(CXXFLAGS) -g $^ -o$@ -L. -lrainback -lpthread

tests/test_websocket: tests/test_websocket.c
	$(CC) $(CXXFLAGS) -g $^ -o$@ -L. -lrainback

clean:
	rm -f librainback.so rainback *.o
	cd servermod && $(MAKE) clean
	cd environment_ws && $(MAKE) clean
.PHONY: clean

clean-certificate: | certificate.pem key.pem
	rm certificate.pem key.pem
.PHONY: clean-certificate

install: librainback.so
	cp $^ $(LIBDIR)
.PHONY: install
