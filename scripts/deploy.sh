#!/bin/bash
# Copies the built programs to the QNX target over SSH.
# Defaults match the QEMU setup (port 2222 forwarded to the guest's SSH).
set -e
cd "$(dirname "$0")/.."
HOST=${TARGET_HOST:-localhost}
PORT=${TARGET_PORT:-2222}
KEY=${TARGET_KEY:-$HOME/.ssh/qnx_vm}
SSH="ssh -i $KEY -p $PORT root@$HOST"

# A running program's file cannot be overwritten on QNX, so stop them first.
$SSH 'slay -s KILL vsbd; sleep 1; slay -s KILL heartbeat; slay -s KILL monitor; slay -s KILL dashboard; slay -s KILL hog; sleep 1' || true

scp -i "$KEY" -P "$PORT" \
    build/vsbd build/monitor build/dashboard build/publisher build/heartbeat build/hog \
    scripts/demo.sh scripts/priority-demo.sh root@"$HOST":/tmp/
$SSH 'chmod +x /tmp/vsbd /tmp/monitor /tmp/dashboard /tmp/publisher /tmp/heartbeat /tmp/hog /tmp/demo.sh /tmp/priority-demo.sh'

echo "DEPLOY OK. Start the bus on the target with: /tmp/vsbd &"
