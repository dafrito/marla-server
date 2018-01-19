PORT=4479
BACKEND_PORT=8081

CXXFLAGS=-I $(HOME)/include -I/usr/include/httpd -I/usr/include/apr-1 -lapr-1 -laprutil-1 -fPIC -L$(HOME)/lib -lparsegraph_user -lparsegraph_List -lparsegraph_environment

all: libenvironment_ws.so
.PHONY: all

libenvironment_ws.so: environment_ws.c prepare.h
	$(CC) -o$@ $(CXXFLAGS) -shared -g `pkg-config --cflags openssl apr-1 ncurses` -I"../src" environment_ws.c -L.. -lmarla

clean:
	rm -f libenvironment_ws.so
.PHONY: clean
