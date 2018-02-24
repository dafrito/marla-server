#!/bin/bash

port=`sed -nre '/^PORT=/s/PORT=//p' Makefile`

if echo $port | grep -v -q ':'; then
    port=localhost:$port
fi

path=$*

if test $# -lt 1; then
    path=/contact
fi

if echo $path | grep -v -q '^/'; then
    path=/$path
fi

for t in `seq 1 99999`; do
    echo $t $port$path
    curl -k $port$path || exit
    sleep 0.025
done
