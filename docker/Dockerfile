# Cross-build environment matching the camera's kernel toolchain (gcc 4.7.3, ARM EABI).
# amd64 on purpose: trusty only ships the 4.7 cross compiler for x86 hosts.
FROM --platform=linux/amd64 ubuntu:14.04
ENV DEBIAN_FRONTEND=noninteractive
# trusty's dpkg sporadically fails to unpack under Rosetta/overlayfs; retry until clean.
RUN apt-get update && \
    for i in 1 2 3 4 5; do \
        apt-get install -y --no-install-recommends \
            make gcc libc6-dev gcc-arm-linux-gnueabi binutils-arm-linux-gnueabi \
            libc6-dev-armel-cross \
            bc git ca-certificates kmod file && break; \
        apt-get install -f -y || true; \
    done && \
    dpkg -l | grep -q '^ii  gcc-4.7-arm-linux-gnueabi' && \
    dpkg -l | grep -q '^ii  libc6-dev-armel-cross' && \
    rm -rf /var/lib/apt/lists/*
ENV ARCH=arm CROSS_COMPILE=arm-linux-gnueabi-
WORKDIR /work
