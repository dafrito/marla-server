#!/bin/bash
gcc -otest-client `pkg-config --cflags --libs apr-1 openssl` test-client.c && ./test-client
