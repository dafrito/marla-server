#!/bin/bash
(gcc -g test_ring.c ../librainback.a -otest_ring -lm && ./test_ring) || exit 1
(gcc -g test_small_ring.c ../librainback.a -otest_small_ring -lm && ./test_small_ring ) || exit 1
(gcc -g test_ring_putback.c ../librainback.a -otest_ring_putback -lm && ./test_ring_putback) || exit 1
