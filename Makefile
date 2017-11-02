all: librainback.a rainback
.PHONY: all

librainback.a: ring.c connection.c client.c | rainback.h
	$(CC) -c -g $^
	ar rcs $@ ring.o connection.o client.o

rainback: epoll.c librainback.a | rainback.h
	$(CC) -g `pkg-config --cflags --libs openssl apr-1 ncurses` $^ -o$@ -lpthread

certificate.pem key.pem:
	openssl req -newkey rsa:2048 -nodes -keyout key.pem -x509 -days 365 -out certificate.pem

PORT=4433

kill: rainback.tmux
	tmux -S rainback.tmux kill-server
.PHONY: kill

run: rainback certificate.pem key.pem
	tmux -S rainback.tmux new-s -d ./rainback $(PORT)
.PHONY: run

tmux:
	tmux -S rainback.tmux att
.PHONY: tmux

check: certificate.pem run-tests
	./test-ring.sh
	./run-tests.sh $(PORT)
	#./run-tests $(PORT)
	./test_low.sh 4433 TEST
.PHONY: check

run-tests: run-tests.c | rainback.h librainback.a
	$(CC) -g `pkg-config --cflags --libs openssl apr-1` $^ -o$@ librainback.a -lpthread

clean:
	rm -f librainback.a rainback *.o
.PHONY: clean

clean-certificate: | certificate.pem key.pem
	rm certificate.pem key.pem
.PHONY: clean-certificate
