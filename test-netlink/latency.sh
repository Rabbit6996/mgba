#!/bin/sh
# usage: latency.sh <one-way delay ms> <jitter ms> <count gap mode childDelay>
cd "$(dirname "$0")"
d=$1; j=$2; shift 2
python3 proxy.py 5740 5738 $d $j &
P=$!
sleep 0.5
THROTTLE=1 ./run.sh lat$d-j$j 5738 5740 "$@"
kill $P
