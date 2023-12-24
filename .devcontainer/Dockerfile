ARG VARIANT="bookworm"
FROM mcr.microsoft.com/devcontainers/base:bookworm

ARG PICO_SDK_VERSION="1.5.0"
ARG VERIBLE_VERSION="v0.0-3471-g9cb45092"

# Use unattended mode for 'apt'
ARG DEBIAN_FRONTEND="noninteractive"

# Required by VS Code "xaver.clang-format" extension
RUN apt-get update && apt-get -y install --no-install-recommends \
    clang-format
#
# Tools for simulating/linting RTL
#

# Install Icarus Verilog
RUN apt-get update && apt-get -y install --no-install-recommends \
    iverilog

# Install Verible
ARG VERIBLE_ARCHIVE=verible-${VERIBLE_VERSION}-linux-static-x86_64.tar.gz
RUN mkdir -p /opt/verible && \
    wget https://github.com/chipsalliance/verible/releases/download/${VERIBLE_VERSION}/${VERIBLE_ARCHIVE} -P /tmp && \
    tar -x -f /tmp/${VERIBLE_ARCHIVE} -C /opt/verible --strip-components 1 && \
    rm -rf /tmp/${VERIBLE_ARCHIVE}
ENV PATH "/opt/verible/bin:${PATH}"

#
# Tools for building/debugging RP2040 binaries
#

# Raspberry Pi Pico SDK
ARG PICO_PATH="/opt/pico"
WORKDIR $PICO_PATH

# We need the arm toolchain for building RP2040 binaries as well as the native toolchain
# in order to build the 'pioasm' and 'elf2uf2' tools.
RUN apt-get update && apt-get -y install --no-install-recommends \
    build-essential \
    cmake \
    gcc-arm-none-eabi \
    gdb-multiarch \
    libnewlib-arm-none-eabi \
    libstdc++-arm-none-eabi-newlib

# Get Raspberry Pi Pico SDK
RUN git clone --depth 1 --branch $PICO_SDK_VERSION https://github.com/raspberrypi/pico-sdk.git && \
    cd pico-sdk && \
    git submodule update --init

ENV PICO_SDK_PATH="${PICO_PATH}/pico-sdk"

# Get Raspberry Pi Pico examples and configure cmake
RUN git clone --depth 1 --branch "sdk-${PICO_SDK_VERSION}" https://github.com/raspberrypi/pico-examples.git && \
    cd pico-examples && \
    mkdir build && \
    cd build && \
    cmake ..

# Build OpenOCD with Picoprobe support
RUN apt-get update && apt-get -y install --no-install-recommends \
    autoconf \
    automake \
    build-essential \
    libftdi-dev \
    libtool \
    libusb-1.0-0-dev \
    pkg-config \
    texinfo

RUN git clone https://github.com/raspberrypi/openocd.git --branch rp2040-v0.12.0 --depth=1 --no-single-branch && \
    cd openocd && \
    ./bootstrap && \
    ./configure && \
    make -j4 && \
    sudo make install

# Install minicom for UART communication with Pico
RUN apt-get update && apt-get -y install --no-install-recommends \
    minicom

# Fix warnings from Cortex-Debug extension
# (See https://forums.raspberrypi.com/viewtopic.php?t=333146)
RUN apt-get update && apt-get -y install --no-install-recommends \
    binutils-multiarch && \
    cd /usr/bin && \
    ln -s /usr/bin/objdump objdump-multiarch && \
    ln -s /usr/bin/nm nm-multiarch
