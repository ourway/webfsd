#!/bin/sh
echo "Content-Type: text/plain"
echo
set | while read line; do echo $line; sleep 1; done
