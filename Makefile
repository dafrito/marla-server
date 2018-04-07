include port.mk
BACKEND_PORT=8081
LOGPORT=28122
MARLAFLAGS=-nossl -db $(HOME)/var/parsegraph/users.sqlite
PACKAGE_NAME=marla
PACKAGE_VERSION=1.3
PACKAGE_RELEASE=8
PACKAGE_SUMMARY=Marla web server
PACKAGE_DESCRIPTION=Marla web server
PACKAGE_URL=rainback.com
build_cpu=x86_64
USER=$(shell whoami)
UID=$(shell id -u `whoami`)
GID=$(shell id -g `whoami`)
PREFIX=/home/$(shell whoami)
BINDIR=$(PREFIX)/bin
LIBDIR=$(PREFIX)/lib
INCLUDEDIR=$(PREFIX)/include
PKGCONFIGDIR=$(LIBDIR)/pkgconfig

CFLAGS=-Og -Wall -g -I$(HOME)/include -I/usr/include/httpd -I/usr/include/apr-1 `pkg-config --cflags openssl apr-1 ncurses` -fPIC
core_LDLIBS=`pkg-config --libs openssl apr-1 ncurses` -ldl
main_LDLIBS=`pkg-config --libs openssl apr-1 ncurses` -lapr-1 -laprutil-1 -L$(HOME)/lib

# Distribute the pkg-config file.
pkgconfigdir = $(libdir)/pkgconfig

pkgconfig_DATA = \
	marla.pc

MOSTLYCLEANFILES = marla.pc

all: src/test_basic src/test-ring.sh src/test-connection.sh src/test_many_requests
	test ! -d $(INCLUDEDIR) || cp src/marla.h $(INCLUDEDIR)
	cd src && ./test_basic
	cd src && ./test-ring.sh
	cd src && ./test-connection.sh $(PORT)
	$(MAKE) marla
	test ! -d ../mod_rainback || (cd ../mod_rainback && $(MAKE))
	test ! -d ../environment_ws || (cd ../environment_ws && $(MAKE));
.PHONY: all

src/test-ring.sh: src/test_ring src/test_small_ring src/test_ring_putback src/test_ring_po2

src/test-connection.sh: src/test_duplex src/test_connection src/test_websocket src/test_chunks src/test_backend

isntall: install
.PHONY: isntall

port.mk:
	echo "PORT=127.0.0.1:4000" >>$@

create_environment: create_environment.c

mod_rainback.so:
	cd ../mod_rainback && ./deploy.sh

BASE_OBJECTS=src/ring.o src/connection.o src/duplex.o src/request.o src/client.o src/log.o src/backend.o src/hooks.o src/ChunkedPageRequest.o src/ssl.o src/cleartext.o src/terminal.o src/server.o src/idler.o src/http.o src/WriteEvent.o src/websocket.o src/file.o

libmarla.so: $(BASE_OBJECTS) src/marla.h
	$(CC) $(CFLAGS) -o$@ -shared -lpthread $(BASE_OBJECTS)

marla: src/main.c libmarla.so src/marla.h Makefile
	$(CC) src/main.c -o$@ -lpthread  -L. -lmarla $(CFLAGS) $(main_LDLIBS)

certificate.pem key.pem:
	openssl req -newkey rsa:2048 -nodes -keyout key.pem -x509 -days 365 -out certificate.pem

kill: marla.tmux
	tmux -S marla.tmux kill-server
.PHONY: kill

run: marla certificate.pem key.pem mod_rainback.so
	tmux -S marla.tmux new-s -d ./marla $(PORT) $(BACKEND_PORT) $(LOGPORT) $(MARLAFLAGS) ./mod_rainback.so?mod_rainback_init
.PHONY: run

strace: marla certificate.pem key.pem mod_rainback.so
	strace ./marla $(PORT) $(BACKEND_PORT) $(LOGPORT) $(MARLAFLAGS) -nocurses ./mod_rainback.so?mod_rainback_init
.PHONY: run

debug: marla certificate.pem key.pem mod_rainback.so
	tmux -S marla.tmux new-s -d gdb ./marla -ex 'r $(PORT) $(BACKEND_PORT) $(LOGPORT) $(MARLAFLAGS) -nocurses ./mod_rainback.so?mod_rainback_init'
.PHONY: debug

valgrind: marla certificate.pem key.pem mod_rainback.so
	valgrind --suppressions=marla.supp ./marla $(PORT) $(BACKEND_PORT) $(LOGPORT) $(MARLAFLAGS) -nocurses ./mod_rainback.so?mod_rainback_init
	#valgrind --leak-check=full --suppressions=marla.supp --show-leak-kinds=all ./marla $(PORT) $(BACKEND_PORT) $(LOGPORT) $(MARLAFLAGS) -nocurses ./mod_rainback.so?mod_rainback_init
.PHONY: valgrind

4400: marla certificate.pem key.pem mod_rainback.so
	tmux -S marla.tmux new-s -d ./marla $@ $(BACKEND_PORT) $(LOGPORT) $(MARLAFLAGS) ./mod_rainback.so?mod_rainback_init
.PHONY: 4400

4401: marla certificate.pem key.pem mod_rainback.so
	tmux -S marla.tmux new-s -d ./marla $@ $(BACKEND_PORT) $(LOGPORT) $(MARLAFLAGS) ./mod_rainback.so?mod_rainback_init
.PHONY: 4401

4402: marla certificate.pem key.pem mod_rainback.so
	tmux -S marla.tmux new-s -d ./marla $@ $(BACKEND_PORT) $(LOGPORT) $(MARLAFLAGS) ./mod_rainback.so?mod_rainback_init
.PHONY: 4402

4403: marla certificate.pem key.pem mod_rainback.so
	tmux -S marla.tmux new-s -d ./marla $@ $(BACKEND_PORT) $(LOGPORT) $(MARLAFLAGS) ./mod_rainback.so?mod_rainback_init
.PHONY: 4403

4404: marla certificate.pem key.pem mod_rainback.so
	tmux -S marla.tmux new-s -d ./marla $@ $(BACKEND_PORT) $(LOGPORT) $(MARLAFLAGS) ./mod_rainback.so?mod_rainback_init
.PHONY: 4404

4405: marla certificate.pem key.pem mod_rainback.so
	tmux -S marla.tmux new-s -d ./marla $@ $(BACKEND_PORT) $(LOGPORT) $(MARLAFLAGS) ./mod_rainback.so?mod_rainback_init
.PHONY: 4405

tmux:
	tmux -S marla.tmux att
.PHONY: tmux

check: certificate.pem src/test_basic src/test_ring src/test_small_ring src/test_ring_putback src/test_connection src/test_websocket src/test_chunks src/test_backend src/test_duplex src/test_many_requests
	cd src || exit; \
	for i in seq 3; do \
	echo Running connecting tests; \
	echo Running low buffer-size tests; \
	./test_low.sh $(PORT) TEST || exit; \
	./test_low_lf.sh $(PORT) TEST || exit; \
	done
.PHONY: check

src/test_basic: src/test_basic.c src/ring.o
	$(CC) $(CFLAGS) -g $^ -o$@ $(core_LDLIBS)

src/test_many_requests: src/test_many_requests.c $(BASE_OBJECTS) src/marla.h Makefile
	$(CC) $(CFLAGS) -g $@.c $(BASE_OBJECTS) -o$@ $(core_LDLIBS)

src/test_ring: src/test_ring.c src/ring.o
	$(CC) $(CFLAGS) -g $^ -o$@ $(core_LDLIBS)

src/test_ring_po2: src/test_ring_po2.c src/ring.o
	$(CC) $(CFLAGS) -g $^ -o$@ $(core_LDLIBS)

src/test_small_ring: src/test_small_ring.c src/ring.o
	$(CC) $(CFLAGS) -g $^ -o$@ $(core_LDLIBS)

src/test_ring_putback: src/test_ring_putback.c src/ring.o
	$(CC) $(CFLAGS) -g $^ -o$@ $(core_LDLIBS)

src/test_connection: src/test_connection.c $(BASE_OBJECTS) src/marla.h Makefile
	$(CC) $(CFLAGS) -g src/test_connection.c $(BASE_OBJECTS) -o$@ $(core_LDLIBS)

src/test_websocket: src/test_websocket.c $(BASE_OBJECTS) src/marla.h Makefile
	$(CC) $(CFLAGS) -g src/test_websocket.c $(BASE_OBJECTS) -o$@ $(core_LDLIBS)

src/test_chunks: src/test_chunks.c $(BASE_OBJECTS) src/marla.h Makefile
	$(CC) $(CFLAGS) -g src/test_chunks.c $(BASE_OBJECTS) -o$@ $(core_LDLIBS)

src/test_backend: src/test_backend.c $(BASE_OBJECTS) src/marla.h Makefile
	$(CC) $(CFLAGS) -g src/test_backend.c $(BASE_OBJECTS) -o$@ $(core_LDLIBS)

src/test_duplex: src/test_duplex.c $(BASE_OBJECTS) src/marla.h Makefile
	$(CC) $(CFLAGS) -g src/test_duplex.c $(BASE_OBJECTS) -o$@ $(core_LDLIBS)

clean:
	rm -f libmarla.so marla *.o src/*.o marla.a
	rm -f src/test_basic src/test_connection src/test_websocket src/test_ring src/test_ring_putback src/test_small_ring test-client src/test_backend src/test_duplex $(PACKAGE_NAME)-$(PACKAGE_VERSION).tar.gz create_environment $(PACKAGE_NAME).spec rpm.sh
	cd ../mod_rainback && $(MAKE) clean
.PHONY: clean

clena: clean
.PHONY: clena

clean-certificate: | certificate.pem key.pem
	rm certificate.pem key.pem
.PHONY: clean-certificate

install: libmarla.so marla src/marla.h marla.pc
	cp libmarla.so $(LIBDIR)
	cp marla $(BINDIR)
	cp src/marla.h $(INCLUDEDIR)
	cp marla.pc $(PKGCONFIGDIR)
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

$(PACKAGE_NAME)-$(PACKAGE_VERSION).tar.gz: src/*.c src/*.h Makefile src/*.sh src/*.html src/*.hreq src/*.hrep logviewer.jar spam.sh
	tar --transform="s'^'$(PACKAGE_NAME)-$(PACKAGE_VERSION)/'g" -cz -f $@ $^

dist-gzip: $(PACKAGE_NAME)-$(PACKAGE_VERSION).tar.gz $(PACKAGE_NAME).spec
.PHONY: dist-gzip

rpm: rpm.sh $(PACKAGE_NAME).spec dist-gzip
	bash $< || exit 1
	test ! -d ../mod_rainback || (cd ../mod_rainback && $(MAKE) rpm)
.PHONY: rpm

marla.pc: marla.pc.head
	cat $< >$@-wip
	sed -i -re 's`@prefix@`$(PREFIX)`g' $@-wip
	sed -i -re 's`@exec_prefix@`$(PREFIX)`g' $@-wip
	sed -i -re 's/@PACKAGE_NAME@/$(PACKAGE_NAME)/g' $@-wip
	sed -i -re 's/@PACKAGE_VERSION@/$(PACKAGE_VERSION)/g' $@-wip
	sed -i -re 's/@PACKAGE_RELEASE@/$(PACKAGE_RELEASE)/g' $@-wip
	sed -i -re 's/@PACKAGE_SUMMARY@/$(PACKAGE_SUMMARY)/g' $@-wip
	sed -i -re 's/@PACKAGE_DESCRIPTION@/$(PACKAGE_DESCRIPTION)/g' $@-wip
	sed -i -re 's/@PACKAGE_URL@/$(PACKAGE_URL)/g' $@-wip
	sed -i -re 's/@build_cpu@/$(build_cpu)/g' $@-wip
	echo "Libs: -L$(LIBDIR) $(main_LDLIBS) -lmarla" >>$@-wip
	echo "Cflags: -I$(INCLUDEDIR) $(CFLAGS)" >>$@-wip
	mv $@-wip $@
