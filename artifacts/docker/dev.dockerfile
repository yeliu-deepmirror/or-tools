# Not performing cleaning for intermediate stages since it does not affect final image size.
FROM ubuntu:20.04 as clang_llvm

ENV PATH /opt/llvm/bin:$PATH
COPY installers/clang_llvm.sh /tmp/installers/
RUN bash -c "source /tmp/installers/clang_llvm.sh && download_clang_llvm"

FROM ubuntu:20.04

COPY installers/apt_install_clean.sh /usr/local/bin/

# Set locale.
RUN apt_install_clean.sh locales && \
    localedef -i en_US -c -f UTF-8 -A /usr/share/locale/locale.alias en_US.UTF-8
ENV LANG en_US.utf8

COPY installers/bazel.sh /tmp/installers/
RUN bash /tmp/installers/bazel.sh && rm /tmp/installers/bazel.sh

RUN apt_install_clean.sh \
    git=1:2.25.1* \
    g++=4:9.3.0* \
    software-properties-common=0.99.9* \
    openjdk-11-jdk=11.0*

ENV PATH /opt/llvm/bin:$PATH
COPY --from=clang_llvm /data /opt
COPY installers/clang_llvm.sh /tmp/installers/
RUN bash -c "source /tmp/installers/clang_llvm.sh && install_clang_llvm /opt/llvm" && \
    rm /tmp/installers/clang_llvm.sh

RUN apt_install_clean.sh \
    python3-dev=3.8.2* \
    python3-pip=20.0.2*
RUN pip3 install cpplint==1.6.0

RUN ldconfig
