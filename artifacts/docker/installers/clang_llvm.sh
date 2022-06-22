#!/usr/bin/env bash

# This is a time consuming step, thus we split it into two steps: download && install.
# Put the downloading and extracting step in an intermediate stage to cut down overall
# building time and avoid size inflation of final image.

export LLVM_VERSION=14.0.0
export LLVM_DISTRO=x86_64-linux-gnu-ubuntu-18.04
export LLVM_RELEASE="clang+llvm-${LLVM_VERSION}-${LLVM_DISTRO}"

function download_clang_llvm() {
  set -e

  apt-get update
  apt-get install -y --no-install-recommends \
    curl=7.68.0* \
    cmake=3.16.3* \
    ninja-build=1.10.0* \
    software-properties-common=0.99.9* \
    xz-utils=5.2.4* \
    python3=3.8.2* \
    g++=4:9.3.0*

  mkdir -p /data

  # Install pre-built clang+llvm package.
  # Check http://releases.llvm.org/download.html.
  curl -LO https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/${LLVM_RELEASE}.tar.xz
  tar Jxf ${LLVM_RELEASE}.tar.xz
  mv ${LLVM_RELEASE} /opt/llvm

  install_clang_llvm /opt/llvm

  # Rebuild both libc++ and libc++abi with MemorySanitizer.
  # Check https://github.com/google/sanitizers/wiki/MemorySanitizerLibcxxHowTo.
  curl -L https://github.com/llvm/llvm-project/archive/llvmorg-${LLVM_VERSION}.tar.gz | tar zx
  cd llvm-project-llvmorg-${LLVM_VERSION}
  mkdir build
  cd build
  cmake -G Ninja \
    -D LLVM_ENABLE_PROJECTS="libcxxabi;libcxx" -D LLVM_USE_LINKER=lld \
    -D LLVM_USE_SANITIZER=Memory -D CMAKE_BUILD_TYPE=Release \
    -D CMAKE_C_COMPILER=clang -D CMAKE_CXX_COMPILER=clang++ \
    -D CMAKE_INSTALL_PREFIX="/data/libcxx_msan" \
    ../llvm
  ninja install-cxx install-cxxabi

  mkdir -p /data/llvm/bin /data/llvm/lib/
  mv /opt/llvm/include /data/llvm/include
  mv /opt/llvm/lib/clang /data/llvm/lib/clang
  # See required binaries in /toolchain/clang.bzl.
  mv /opt/llvm/bin/clang \
     /opt/llvm/bin/clang-$(echo $LLVM_VERSION | cut -d. -f1) \
     /opt/llvm/bin/clang-cpp \
     /opt/llvm/bin/ld64.lld \
     /opt/llvm/bin/ld.lld \
     /opt/llvm/bin/lld \
     /opt/llvm/bin/llvm-ar \
     /opt/llvm/bin/llvm-cov \
     /opt/llvm/bin/llvm-nm \
     /opt/llvm/bin/llvm-objcopy \
     /opt/llvm/bin/llvm-objdump \
     /opt/llvm/bin/llvm-strip \
     /data/llvm/bin/
}

function install_clang_llvm() {
  set -e

  LLVM_PATH=$1
  ln -s ${LLVM_VERSION} ${LLVM_PATH}/lib/clang/current
  echo "${LLVM_PATH}/lib" > /etc/ld.so.conf.d/llvm.conf
  echo "${LLVM_PATH}/lib/clang/current/lib/linux" >> /etc/ld.so.conf.d/llvm.conf
  ldconfig
}
