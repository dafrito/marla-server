#!/bin/bash

die() {
    echo $* >&2
    exit 1
}
test $# -ge 2 || die "Usage: $0 <port> <test-name>"
port=$1
name=$2
test -f $name.hreq || die "No $name.hreq test found."
nc localhost $port --ssl -C --ssl-verify --ssl-trustfile=certificate.pem -c "unix2dos <$name.hreq" -i 5 -o $name.hrep
