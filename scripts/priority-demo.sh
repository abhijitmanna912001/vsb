#!/bin/sh
# Shows that a high-priority monitor keeps its wake-up deadline under CPU load.
# Run on the QNX target with vsbd already running (/tmp/vsbd &).
# Takes about one minute. Prints the last two statistics lines of each phase.
T=/tmp

phase() {
    title=$1
    shift
    $T/monitor -s "$@" > $T/phase.out &
    sleep 13
    slay monitor
    echo "== $title"
    tail -n 2 $T/phase.out
}

slay -f hog 2>/dev/null

phase "monitor at priority 10, idle CPU"

echo "-- starting 3 CPU hogs at priority 10"
$T/hog &
$T/hog &
$T/hog &
sleep 2

phase "monitor at priority 10, under load"
phase "monitor at priority 40, under load" -p 40

slay -f hog
echo "== done"
