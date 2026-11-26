#!/bin/sh
# Automated scenario tests. Runs on the QNX target, starts its own vsbd,
# monitor and heartbeat, drives signals through /dev/vsb and checks the
# published "fault" signal. Exit status is 0 only if every check passes.
T=/tmp
PASS=0
FAIL=0

stop_all() {
    slay -f heartbeat monitor dashboard hog vsbd 2>/dev/null
}

# expect <description> <expected fault>
expect() {
    sleep 1
    got=`cat /dev/vsb | grep '^fault=' | head -n 1`
    if [ "$got" = "fault=$2" ]; then
        echo "PASS  $1"
        PASS=`expr $PASS + 1`
    else
        echo "FAIL  $1 (expected fault=$2, got '$got')"
        FAIL=`expr $FAIL + 1`
    fi
}

stop_all
sleep 1
$T/vsbd &
sleep 1
$T/monitor > $T/test-monitor.log &
$T/heartbeat &
sleep 2

expect "starts in OK" OK

echo "speed=120" > /dev/vsb
expect "speed 120 raises OVERSPEED" OVERSPEED

echo "speed=80" > /dev/vsb
expect "speed back to 80 clears the fault" OK

echo "brake=4" > /dev/vsb
expect "brake 4 at speed 80 raises HARD_BRAKE" HARD_BRAKE

echo "brake=0" > /dev/vsb
expect "releasing the brake clears the fault" OK

echo "speed=30" > /dev/vsb
echo "brake=4" > /dev/vsb
expect "hard brake at speed 30 is below the floor, stays OK" OK

echo "bad input" > /dev/vsb 2>/dev/null
expect "bad input does not change the fault" OK
if cat /dev/vsb | grep -q "bad"; then
    echo "FAIL  bad input was stored in the signal table"
    FAIL=`expr $FAIL + 1`
else
    echo "PASS  bad input was not stored"
    PASS=`expr $PASS + 1`
fi

echo "brake=0" > /dev/vsb
echo "speed=0" > /dev/vsb
slay -f heartbeat
sleep 3
expect "heartbeat stops, watchdog raises SENSOR_TIMEOUT" SENSOR_TIMEOUT

echo "speed=120" > /dev/vsb
expect "SENSOR_TIMEOUT takes priority over OVERSPEED" SENSOR_TIMEOUT

$T/heartbeat &
sleep 1
expect "heartbeat returns, OVERSPEED is visible again" OVERSPEED

echo "speed=0" > /dev/vsb
expect "speed 0 returns to OK" OK

stop_all
echo
echo "$PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
