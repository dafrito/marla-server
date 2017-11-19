#!/bin/bash
(gcc -g test_ring.c ../librainback.a -otest_ring `pkg-config --cflags --libs apr-1 openssl` -lm && ./test_ring) || exit 1
(gcc -g test_small_ring.c ../librainback.a -otest_small_ring `pkg-config --cflags --libs apr-1 openssl` -lm && ./test_small_ring ) || exit 1
(gcc -g test_ring_putback.c ../librainback.a -otest_ring_putback `pkg-config --cflags --libs apr-1 openssl` -lm && ./test_ring_putback) || exit 1
