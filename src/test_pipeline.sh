#!/bin/bash

die() {
    echo $* >&2
    exit 1
}
test $# -ge 2 || die "Usage: $0 <port> <test-name>"
port=$1
name=$2
test -f $name.hreq || die "No $name.hreq test found."
cat $name.hreq - | socat SYSTEM:./feed.sh openssl:localhost:$port,cafile=certificate.pem
