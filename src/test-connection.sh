#!/bin/bash

TMPDIR=/tmp

for tester in test_duplex test_connection test_chunks test_websocket test_backend test_many_requests; do
    #./$tester $* || exit 1
    ./$tester $* >$TMPDIR/marla-test.log 2>&1 || (cat $TMPDIR/marla-test.log; exit 1)
done
