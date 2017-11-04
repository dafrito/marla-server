#!/bin/bash

echo -en "GET / HTTP/1.1\r\n"
echo -en "Host: localhost:$port\r\n"
echo -en "\r\n"

IFS='
'
read statusline
while true; do
    read line
    echo $line>>REP
done
