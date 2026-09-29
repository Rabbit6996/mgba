#!/bin/sh
# usage: run.sh <label> <host port> <join port> <count gap mode childDelay>
cd "$(dirname "$0")"
label=$1; hport=$2; jport=$3; shift 3
./netlink-test host rom/linktest.gba "$@" $hport > "out-$label-host.log" 2>&1 &
H=$!
sleep 0.3
./netlink-test join rom/linktest.gba 127.0.0.1 "$@" $jport > "out-$label-join.log" 2>&1
J=$?
wait $H
HS=$?
echo "== $label (host=$HS join=$J)"
grep -h "^\[" "out-$label-host.log" "out-$label-join.log" | grep -v "\] connected"
grep -h "log\[" "out-$label-host.log" "out-$label-join.log" | grep -v "established\|partner game" | sort | uniq -c | head -5
