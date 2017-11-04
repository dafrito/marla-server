#!/bin/bash
(gcc -g test_connection.c ../librainback.a -otest_connection -lm `pkg-config --libs openssl` && ./test_connection) || exit 1
