#!/bin/bash

# The directory contains current script.
DIR=$(dirname $(realpath "$BASH_SOURCE"))
# The directory contains .git directory.
REPO_DIR=$DIR/../../

# Start qemu emulator
docker run --rm --privileged multiarch/qemu-user-static --reset -p yes

docker build -t or-tools-dev \
    -f $REPO_DIR/artifacts/docker/dev.dockerfile \
    --build-arg ARCH=amd64 \
    $REPO_DIR/artifacts/docker
