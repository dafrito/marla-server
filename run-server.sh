#!/bin/bash

make
strace ./rainback 4003 8081 servermod/libservermod.so?module_servermod_init environment_ws/libenvironment_ws.so?module_environment_ws_init
