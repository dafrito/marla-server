#!/bin/bash
./test_ring || exit 1
./test_small_ring || exit 1
./test_ring_putback || exit 1
