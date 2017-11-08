#!/bin/bash
gcc -otest-client `pkg-config --cflags --libs openssl` test-client.c && ./test-client
