#!/bin/bash
(gcc -g test_connection.c ../librainback.a -otest_connection -lm `pkg-config --cflags --libs openssl apr-1` && ./test_connection $*) || exit 1
