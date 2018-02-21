#!/bin/bash


for tester in test_duplex test_connection test_chunks test_websocket test_backend test_many_requests; do
    #./$tester $* || exit 1
    ./$tester $* >$HOME/tmp/test.log 2>&1 || (cat $HOME/tmp/test.log; exit 1)
done
