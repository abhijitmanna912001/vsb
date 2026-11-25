#!/bin/sh
# Drives a short "drive cycle" through the vehicle signal bus.
# Run inside QNX while vsbd, monitor and dashboard are running.
P=/tmp/publisher

slay heartbeat 2>/dev/null

echo "== Starting heartbeat sensor"
/tmp/heartbeat &
sleep 3

echo "== Idle"
$P speed 0 0 100
$P brake 0 0 100
sleep 2

echo "== Accelerating to 90"
$P speed 0 90 50
sleep 1

echo "== Overspeed: pushing past 100"
$P speed 90 110 200
sleep 2

echo "== Recovering to 70"
$P speed 110 70 80
sleep 1

echo "== Hard braking"
$P brake 0 3 400
sleep 2

echo "== Releasing brake and stopping"
$P brake 3 0 200
$P speed 70 0 60
sleep 1

echo "== Sensor failure: heartbeat stops"
slay heartbeat
sleep 4

echo "== Sensor recovered"
/tmp/heartbeat &
sleep 3
slay heartbeat

echo "== Demo complete"
