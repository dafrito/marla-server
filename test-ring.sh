#!/bin/bash
(gcc -g ring.c test_ring.c -otest_ring -lm && ./test_ring) || exit 1
(gcc -g ring.c test_small_ring.c -otest_small_ring -lm && ./test_small_ring ) || exit 1
(gcc -g ring.c test_ring_putback.c -otest_ring_putback -lm && ./test_ring_putback) || exit 1
