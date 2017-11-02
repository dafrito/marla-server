#!/bin/bash
while true; do
    ./deploy.sh
    sleep 1
    inotifywait -e modify -r *.c *.h Makefile --format '%w %e' | read file event;
done
