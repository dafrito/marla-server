#!/bin/bash
while true; do
    ./deploy.sh
    sleep 1
    inotifywait -e modify -r src/*.c src/*.h servermod/*.c servermod/Makefile tests tests/*.c tests/*.h tests/*.sh Makefile --format '%w %e' | read file event;
done
