#!/bin/bash

make kill || exit
make check || exit
cp rainback ../server
make run
