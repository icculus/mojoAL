#!/bin/bash

set -e
set -x

gcc -Wall -O0 -ggdb3 -I. -shared -fPIC -o libaltrace_record.so.1 altrace_record.c -ldl
gcc -Wall -O0 -ggdb3 -I. -o altrace_cli altrace_cli.c -ldl