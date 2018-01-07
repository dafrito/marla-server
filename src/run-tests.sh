#!/bin/bash

PORT=$*
TESTFILE=test.html

die() {
echo $*
exit 1
}

test -n $PORT || die "Port number must be provided."

test_connection()
{
    curl -Ss -v -o $TESTFILE --cacert `pwd`/certificate.pem https://localhost:$PORT || exit
    rm $TESTFILE || (echo "Failed to remove test file at $TESTFILE"; exit 1)
}

test_insecure_connection()
{
    curl -s -o $TESTFILE https://localhost:$PORT && (echo "Insecure connection succeeded anyway."; exit 1)
}

test_cleartext_connection()
{
    curl -s -o $TESTFILE http://localhost:$PORT && (echo Cleartext connection seemed to succeed.; exit 1)
}

TESTNO=0
for testfunc in test_connection test_insecure_connection test_cleartext_connection; do
    echo $testfunc
    let TESTNO++
    $testfunc
done
echo "All $TESTNO tests ran successfully."
