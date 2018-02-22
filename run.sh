#!/bin/sh

next_server_port() {
port=`sed -nre '/^PORT=/s/PORT=//p' Makefile`
host=localhost
if echo $port | grep -q ':'; then
    host=`echo $port | grep -Eoe '^[^:]+'`
    port=`echo $port | sed -nre 's/^[^:]+://p'`
fi
port=$(($port + 1))
sed -i -re "s/^PORT=.+$/PORT=$host:$port/" Makefile
}

run_server() {
starttime=`date +%s`
make run
donetime=`date +%s`
if test $(($starttime - $donetime)) -lt 1; then
    make kill
    next_server_port
    make run
    authority=`sed -nre '/^PORT=/s/PORT=//p' Makefile`
    echo "Running on $authority"
    echo $authority | xsel -b -i
else
    exit
fi
}

run_server
