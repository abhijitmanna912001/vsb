#!/bin/bash
# macOS helper: runs build.sh inside the x86_64 Docker container that has the
# QNX SDP (see https://gitlab.com/qnx/projects/docker-mac).
set -e
docker start qnx-build >/dev/null
docker exec -it qnx-build bash -c \
  'source $HOME/qnx800/qnxsdp-env.sh && cd $HOME/qnxprojects/vsb && ./build.sh'
