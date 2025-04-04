# Default to latest Ubuntu LTS.
# (See https://hub.docker.com/r/microsoft/devcontainers-base)
ARG VARIANT="ubuntu"
FROM --platform=linux/amd64 mcr.microsoft.com/devcontainers/base:${VARIANT}

# From https://github.com/raspberrypi/pico-sdk/releases
ARG PICO_SDK_VERSION="2.0.0"

# From https://github.com/raspberrypi/openocd/branches
ARG OPENOCD_VERSION="sdk-2.0.0"

# From https://github.com/chipsalliance/verible/releases
ARG VERIBLE_VERSION="v0.0-3958-g7aae5c08"

# From https://github.com/steveicarus/iverilog/releases
ARG IVERILOG_VERSION="v12_0"

# Efinity software requires a free license from Efinix. Please do the right thing.
# From https://www.efinixinc.com/support/efinity.php
ARG EFINITY_MAJOR_MINOR="2024.2"
ARG EFINITY_PATCH="294"

#
# Install apt package dependencies.
#
# For VS Code "xaver.clang-format" extension:
# 
#   clang-format
#
# Icarus Verilog build dependencies:
#
#   autoconf
#   bison
#   flex
#   g++
#   gcc
#   gperf
#   make
#
# GtkWave:
#
#   gtkwave
#
# RP2040 binaries toolchain, plus native toolchain to build 'pioasm' and 'elf2uf2' tools.
# (See Appendix C: https://datasheets.raspberrypi.com/pico/getting-started-with-pico.pdf)
#
#   build-essential
#   check
#   cmake
#   gcc-arm-none-eabi
#   gdb-multiarch
#   libnewlib-arm-none-eabi
#   libstdc++-arm-none-eabi-newlib
#   ninja-build
#
# OpenOCD build dependencies:
#
#   autoconf
#   automake
#   build-essential
#   libftdi-dev
#   libtool
#   libusb-1.0-0-dev
#   pkg-config
#   texinfo
#
# Verilog-HDL/SystemVerilog/Bluespec SystemVerilog extension dependencies
#
#   universal-ctags
#   verilator
#
# For UART communication with Pico
#
#   minicom
#
# Install CC65 toolchain for 6502 development
#
#   cc65
#
# To fix warnings from Cortex-Debug extension:
# (See https://forums.raspberrypi.com/viewtopic.php?t=333146)
#
#   binutils-multiarch
#

# Use unattended mode for 'apt'
ARG DEBIAN_FRONTEND="noninteractive"
RUN apt-get update && apt-get -y install --no-install-recommends \
    autoconf \
    automake \
    binutils-multiarch \
    bison \
    build-essential \
    cc65 \
    check \
    clang-format \
    cmake \
    flex \
    g++ \
    gcc \
    gcc-arm-none-eabi \
    gdb-multiarch \
    gperf \
    gtkwave \
    libftdi-dev \
    libnewlib-arm-none-eabi \
    libstdc++-arm-none-eabi-newlib \
    libtool \
    libusb-1.0-0-dev \
    make \
    minicom \
    ninja-build \
    pkg-config \
    texinfo \
    universal-ctags \
    verilator

# Clean apt cache and temporary files
RUN apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Cache large downloads:

# Download Efinity software -- Requires a free license from Efinix. Please do the right thing.
# (See: https://www.efinixinc.com/support/efinity.php)
ARG EFINITY_ARCHIVE="efinity-${EFINITY_MAJOR_MINOR}.${EFINITY_PATCH}-ubuntu-x64.tar.bz2"
RUN wget https://www.efinixinc.com/dl/${EFINITY_ARCHIVE} -P /tmp

# Clone Raspberry Pi Pico SDK
# (See https://datasheets.raspberrypi.com/pico/getting-started-with-pico.pdf)
ARG PICO_PATH="/opt/pico"
ENV PICO_SDK_PATH="${PICO_PATH}/pico-sdk"
RUN mkdir -p ${PICO_PATH} && \
    cd ${PICO_PATH} && \
    git clone --branch "${PICO_SDK_VERSION}" --depth 1 --single-branch https://github.com/raspberrypi/pico-sdk.git && \
    cd ${PICO_SDK_PATH} && \
    git submodule update --init --recursive --depth 1 --single-branch

# Clone Raspberry Pi Pico SDK examples
RUN cd ${PICO_PATH} && \
    git clone --branch "sdk-${PICO_SDK_VERSION}" --depth 1 --single-branch https://github.com/raspberrypi/pico-examples.git

# Clone fork of OpenOCD for debugging RP2040
RUN cd ${PICO_PATH} && \
    git clone --branch "${OPENOCD_VERSION}" --depth 1 --single-branch https://github.com/raspberrypi/openocd.git

# Clone Icarus Verilog (built from source)
RUN cd /tmp && \
    git clone --branch ${IVERILOG_VERSION} --depth 1 --single-branch https://github.com/steveicarus/iverilog.git

ARG VERIBLE_ARCHIVE="verible-${VERIBLE_VERSION}-linux-static-x86_64.tar.gz"
RUN wget https://github.com/chipsalliance/verible/releases/download/${VERIBLE_VERSION}/${VERIBLE_ARCHIVE} -P /tmp

# Fix warnings from Cortex-Debug extension
# (See https://forums.raspberrypi.com/viewtopic.php?t=333146)
RUN cd /usr/bin && \
    ln -s /usr/bin/objdump objdump-multiarch && \
    ln -s /usr/bin/nm nm-multiarch

#
# Build and install the tools
#

# Install Icarus Verilog (built from source)
# (Do this early as it takes a while).
RUN cd /tmp && \
    cd iverilog && \
    sh autoconf.sh && \
    sh configure && \
    make -j$(nproc) && \
    make install && \
    cd /tmp && \
    rm -rf iverilog

# Install OpenOCD (built from source for RP2040)
# (See: https://datasheets.raspberrypi.com/pico/getting-started-with-pico.pdf)
RUN cd "${PICO_PATH}/openocd" && \
    ./bootstrap && \
    ./configure && \
    make -j$(nproc) && \
    sudo make install

# Install Efinity -- Requires a free license from Efinix. Please do the right thing.
# (See: https://www.efinixinc.com/support/efinity.php)
RUN tar -xjvf /tmp/${EFINITY_ARCHIVE} -C /opt && \
    rm -rf /tmp/${EFINITY_ARCHIVE} && \
    chmod +x /opt/efinity/${EFINITY_MAJOR_MINOR}/bin/setup.sh

# Install Verible (download from chipsalliance)
RUN mkdir -p /opt/verible && \
    tar -x -f /tmp/${VERIBLE_ARCHIVE} -C /opt/verible --strip-components 1 && \
    rm -rf /tmp/${VERIBLE_ARCHIVE}
ENV PATH="/opt/verible/bin:${PATH}"
