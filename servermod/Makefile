PORT=4479
BACKEND_PORT=8081

CXXFLAGS=-I $(HOME)/include -I/usr/include/httpd -I/usr/include/apr-1 -lapr-1 -laprutil-1 -fPIC -L$(HOME)/lib -lparsegraph_user -lparsegraph_List -lparsegraph_environment

all: libservermod.so
.PHONY: all

libservermod.so: servermod.c
	$(CC) -o$@ $(CXXFLAGS) -shared -g `pkg-config --cflags openssl apr-1 ncurses` -I"../src" $^

clean:
	rm -f libservermod.so
.PHONY: clean
