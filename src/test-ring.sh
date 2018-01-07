#!/bin/bash
./test_ring || exit 1
./test_small_ring || exit 1
./test_ring_putback || exit 1
./test_ring_po2 16 || exit 1
./test_ring_po2 15 2>/dev/null || exit 0
