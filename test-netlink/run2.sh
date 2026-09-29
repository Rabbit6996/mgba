#!/bin/sh
# usage: run2.sh <label> "<host env>" "<join env>" <count gap mode childDelay>
cd "$(dirname "$0")"
label=$1; henv=$2; jenv=$3; shift 3
env $henv ./netlink-test host rom/linktest.gba "$@" > "out-$label-host.log" 2>&1 &
H=$!
sleep 0.3
env $jenv ./netlink-test join rom/linktest.gba 127.0.0.1 "$@" > "out-$label-join.log" 2>&1
J=$?
wait $H
HS=$?
echo "== $label (host=$HS join=$J)"
grep -h "^\[" "out-$label-host.log" "out-$label-join.log" | grep -v "\] connected"
