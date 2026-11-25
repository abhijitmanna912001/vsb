#!/bin/bash
# Builds all programs for QNX. Requires the QNX SDP environment to be loaded:
#   source ~/qnx800/qnxsdp-env.sh
# Override the compiler for another target, e.g. CXX=ntox86_64-g++ ./build.sh
set -e
cd "$(dirname "$0")"
CXX=${CXX:-ntoaarch64-g++}
mkdir -p build
for p in vsbd monitor dashboard publisher; do
    echo "building $p"
    $CXX -O2 -Wall -o build/$p src/$p.cpp
done
echo "BUILD OK"
