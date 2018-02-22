#!/bin/bash

if test $# -lt 1; then
    echo "The serverport must be provided" >&2
    exit 1
fi

for t in `seq 1 10000`; do
    echo $t
    curl localhost:$*/contact
    sleep 0.05
done
