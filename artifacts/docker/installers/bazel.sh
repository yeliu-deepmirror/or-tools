#!/usr/bin/env bash

# https://docs.bazel.build/versions/main/install-ubuntu.html#install-with-installer-ubuntu

set -e

apt_install_clean.sh wget unzip

mkdir -p /tmp/installers
pushd /tmp/installers

BAZEL_VERSION=5.1.1

wget --no-check-certificate https://github.com/bazelbuild/bazel/releases/download/$BAZEL_VERSION/bazel-$BAZEL_VERSION-installer-linux-x86_64.sh
chmod +x bazel-$BAZEL_VERSION-installer-linux-x86_64.sh
./bazel-$BAZEL_VERSION-installer-linux-x86_64.sh
echo "source /usr/local/lib/bazel/bin/bazel-complete.bash" >> ~/.bashrc

rm -rf bazel-$BAZEL_VERSION-installer-linux-x86_64.sh

popd
