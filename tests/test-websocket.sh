#!/bin/bash
(gcc -g test_websocket.c ../librainback.a -otest_websocket -lm `pkg-config --cflags --libs openssl apr-1` && ./test_websocket $*) || exit 1
