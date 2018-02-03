PORT=4546
BACKEND_PORT=8081
LOGPORT=28122
PREFIX=/home/$(shell whoami)
BINDIR=$(PREFIX)/bin
LIBDIR=$(PREFIX)/lib
INCLUDEDIR=$(PREFIX)/include
MARLAFLAGS=-nossl
USER=$(shell whoami)
UID=$(shell id -u `whoami`)
GID=$(shell id -g `whoami`)
PACKAGE_NAME=marla
PACKAGE_VERSION=1.0
PACKAGE_RELEASE=1
PACKAGE_SUMMARY=Marla web server
PACKAGE_DESCRIPTION=Marla web server
PACKAGE_URL=rainback.com
build_cpu=x86_64

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

BASE_OBJECTS=src/ring.o src/connection.o src/request.o src/client.o src/log.o src/backend.o src/hooks.o src/ChunkedPageRequest.o src/ssl.o src/cleartext.o src/terminal.o src/server.o

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
	tmux -S marla.tmux new-s -d ./marla $(PORT) $(BACKEND_PORT) $(LOGPORT) $(MARLAFLAGS) servermod/libservermod.so?module_servermod_init environment_ws/libenvironment_ws.so?module_environment_ws_init
.PHONY: run

debug: marla certificate.pem key.pem servermod/libservermod.so environment_ws/libenvironment_ws.so
	tmux -S marla.tmux new-s -d gdb ./marla -ex 'r $(PORT) $(BACKEND_PORT) $(LOGPORT) $(MARLAFLAGS) -nocurses servermod/libservermod.so?module_servermod_init environment_ws/libenvironment_ws.so?module_environment_ws_init'
.PHONY: debug

valgrind: marla certificate.pem key.pem servermod/libservermod.so environment_ws/libenvironment_ws.so
	valgrind --leak-check=full ./marla $(PORT) $(BACKEND_PORT) $(LOGPORT) $(MARLAFLAGS) -nocurses servermod/libservermod.so?module_servermod_init environment_ws/libenvironment_ws.so?module_environment_ws_init
.PHONY: valgrind

4400: marla certificate.pem key.pem servermod/libservermod.so environment_ws/libenvironment_ws.so
	tmux -S marla.tmux new-s -d ./marla $@ $(BACKEND_PORT) $(LOGPORT) $(MARLAFLAGS) servermod/libservermod.so?module_servermod_init environment_ws/libenvironment_ws.so?module_environment_ws_init
.PHONY: 4400

4401: marla certificate.pem key.pem servermod/libservermod.so environment_ws/libenvironment_ws.so
	tmux -S marla.tmux new-s -d ./marla $@ $(BACKEND_PORT) $(LOGPORT) $(MARLAFLAGS) servermod/libservermod.so?module_servermod_init environment_ws/libenvironment_ws.so?module_environment_ws_init
.PHONY: 4401

4402: marla certificate.pem key.pem servermod/libservermod.so environment_ws/libenvironment_ws.so
	tmux -S marla.tmux new-s -d ./marla $@ $(BACKEND_PORT) $(LOGPORT) $(MARLAFLAGS) servermod/libservermod.so?module_servermod_init environment_ws/libenvironment_ws.so?module_environment_ws_init
.PHONY: 4402

4403: marla certificate.pem key.pem servermod/libservermod.so environment_ws/libenvironment_ws.so
	tmux -S marla.tmux new-s -d ./marla $@ $(BACKEND_PORT) $(LOGPORT) $(MARLAFLAGS) servermod/libservermod.so?module_servermod_init environment_ws/libenvironment_ws.so?module_environment_ws_init
.PHONY: 4403

4404: marla certificate.pem key.pem servermod/libservermod.so environment_ws/libenvironment_ws.so
	tmux -S marla.tmux new-s -d ./marla $@ $(BACKEND_PORT) $(LOGPORT) $(MARLAFLAGS) servermod/libservermod.so?module_servermod_init environment_ws/libenvironment_ws.so?module_environment_ws_init
.PHONY: 4404

4405: marla certificate.pem key.pem servermod/libservermod.so environment_ws/libenvironment_ws.so
	tmux -S marla.tmux new-s -d ./marla $@ $(BACKEND_PORT) $(LOGPORT) $(MARLAFLAGS) servermod/libservermod.so?module_servermod_init environment_ws/libenvironment_ws.so?module_environment_ws_init
.PHONY: 4405

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

install: libmarla.so marla src/marla.h
	cp libmarla.so $(LIBDIR)
	cp marla $(BINDIR)
	cp src/marla.h $(INCLUDEDIR)
.PHONY: install

rpm.sh: rpm.sh.in
	cp -f $< $@-wip
	sed -i -re 's/@PACKAGE_NAME@/$(PACKAGE_NAME)/g' $@-wip
	sed -i -re 's/@PACKAGE_VERSION@/$(PACKAGE_VERSION)/g' $@-wip
	sed -i -re 's/@PACKAGE_RELEASE@/$(PACKAGE_RELEASE)/g' $@-wip
	mv $@-wip $@
	chmod +x rpm.sh

$(PACKAGE_NAME).spec: rpm.spec.in
	cp -f $< $@-wip
	sed -i -re 's/@PACKAGE_NAME@/$(PACKAGE_NAME)/g' $@-wip
	sed -i -re 's/@PACKAGE_VERSION@/$(PACKAGE_VERSION)/g' $@-wip
	sed -i -re 's/@PACKAGE_RELEASE@/$(PACKAGE_RELEASE)/g' $@-wip
	sed -i -re 's/@PACKAGE_SUMMARY@/$(PACKAGE_SUMMARY)/g' $@-wip
	sed -i -re 's/@PACKAGE_DESCRIPTION@/$(PACKAGE_DESCRIPTION)/g' $@-wip
	sed -i -re 's/@PACKAGE_URL@/$(PACKAGE_URL)/g' $@-wip
	sed -i -re 's/@build_cpu@/$(build_cpu)/g' $@-wip
	mv $@-wip $@
	chmod +x $@

$(PACKAGE_NAME)-$(PACKAGE_VERSION).tar.gz: src/*.c src/*.h servermod/*.c servermod/Makefile environment_ws/prepare.h environment_ws/environment_ws.c environment_ws/Makefile Makefile src/*.sh src/*.html src/*.hreq src/*.hrep logviewer.jar
	tar --transform="s'^'$(PACKAGE_NAME)-$(PACKAGE_VERSION)/'g" -cz -f $@ $^

dist-gzip: $(PACKAGE_NAME)-$(PACKAGE_VERSION).tar.gz $(PACKAGE_NAME).spec
.PHONY: dist-gzip

rpm: rpm.sh $(PACKAGE_NAME).spec dist-gzip
	bash $<
.PHONY: rpm

