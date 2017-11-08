all: librainback.a rainback
.PHONY: all

librainback.a: src/ring.c src/connection.c src/client.c src/request.c | src/rainback.h
	$(CC) -c -g $^
	ar rcs $@ ring.o connection.o client.o request.o

rainback: src/epoll.c librainback.a | src/rainback.h
	$(CC) -g `pkg-config --cflags --libs openssl apr-1 ncurses` $^ -o$@ -lpthread

certificate.pem key.pem:
	openssl req -newkey rsa:2048 -nodes -keyout key.pem -x509 -days 365 -out certificate.pem

PORT=4434

kill: rainback.tmux
	tmux -S rainback.tmux kill-server
.PHONY: kill

run: rainback certificate.pem key.pem
	tmux -S rainback.tmux new-s -d ./rainback $(PORT)
.PHONY: run

tmux:
	tmux -S rainback.tmux att
.PHONY: tmux

check: certificate.pem tests/run-tests
	cd tests || exit; \
	./test-ring.sh || exit; \
	./test-connection.sh $(PORT) || exit; \
	for i in seq 3; do \
	./run-tests.sh $(PORT) || exit; \
	./test_low.sh $(PORT) TEST || exit; \
	./test_low_lf.sh $(PORT) TEST || exit; \
	done
.PHONY: check

tests/run-tests: tests/run-tests.c | src/rainback.h librainback.a
	$(CC) -g `pkg-config --cflags --libs openssl apr-1` $^ -o$@ librainback.a -lpthread

clean:
	rm -f librainback.a rainback *.o
.PHONY: clean

clean-certificate: | certificate.pem key.pem
	rm certificate.pem key.pem
.PHONY: clean-certificate
