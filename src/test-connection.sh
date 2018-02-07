#!/bin/bash
./test_duplex $* || exit 1
./test_connection $* || exit 1
./test_chunks $* || exit 1
./test_websocket $* || exit 1
./test_backend $* || exit 1
