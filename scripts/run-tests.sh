#!/bin/bash
# Deploys the build to the QNX target and runs scripts/test.sh there.
# Needs the target running (see scripts/run-qemu.sh) and a fresh ./build.sh.
set -e
cd "$(dirname "$0")/.."
HOST=${TARGET_HOST:-localhost}
PORT=${TARGET_PORT:-2222}
KEY=${TARGET_KEY:-$HOME/.ssh/qnx_vm}

scripts/deploy.sh
ssh -i "$KEY" -p "$PORT" root@"$HOST" /tmp/test.sh
