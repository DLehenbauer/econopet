# Default to latest Ubuntu LTS.
# (See https://hub.docker.com/r/microsoft/devcontainers-base)
ARG VARIANT="ubuntu"
ARG PLATFORM="linux/amd64"
FROM --platform=${PLATFORM} mcr.microsoft.com/devcontainers/base:${VARIANT}

# From https://github.com/raspberrypi/pico-sdk/releases
#  and https://github.com/raspberrypi/picotool/releases
ARG PICO_SDK_VERSION="2.1.1"

# From https://github.com/raspberrypi/openocd/branches
ARG OPENOCD_VERSION="sdk-2.0.0"

# From https://github.com/chipsalliance/verible/releases
ARG VERIBLE_VERSION="v0.0-4007-g98bdb38a"

# From https://github.com/verilator/verilator/releases
ARG VERILATOR_VERSION="v5.030"

# From https://github.com/steveicarus/iverilog/releases
ARG IVERILOG_VERSION="v12_0"

# Efinity software requires a free license from Efinix. Please do the right thing.
# From https://www.efinixinc.com/support/
ARG EFINITY_MAJOR_MINOR="2025.1"
ARG EFINITY_PATCH="110"

# Cache large downloads:

# Download Efinity software -- Requires a free license from Efinix. Please do the right thing.
#
#   1. Create an account on the Efinix website.
#   2. Log in to your account.
#   3. Navigate to the Efinity page to request your free license.
#      (See: https://www.efinixinc.com/support/)
#
ARG EFINITY_ARCHIVE="efinity-${EFINITY_MAJOR_MINOR}.${EFINITY_PATCH}-linux-x64.tar.bz2"
RUN wget https://www.efinixinc.com/dl/${EFINITY_ARCHIVE} -P /tmp

# Clone Raspberry Pi Pico SDK
# (See https://datasheets.raspberrypi.com/pico/getting-started-with-pico.pdf)
ENV PICO_SDK_PATH="/opt/pico-sdk"
RUN cd /opt && \
    git clone --branch "${PICO_SDK_VERSION}" --depth 1 --single-branch https://github.com/raspberrypi/pico-sdk.git && \
    cd ${PICO_SDK_PATH} && \
    git submodule update --init --recursive --depth 1 --single-branch

# Clone 'picotool': Pico SDK recommends installing this tool separately.
RUN cd /tmp && \
    git clone --branch "${PICO_SDK_VERSION}" --depth 1 --single-branch https://github.com/raspberrypi/picotool.git

# Clone fork of OpenOCD for debugging RP2040
RUN cd /tmp && \
    git clone --branch "${OPENOCD_VERSION}" --depth 1 --single-branch https://github.com/raspberrypi/openocd.git

# Clone Verilator (built from source)
RUN cd /tmp && \
    git clone --branch ${VERILATOR_VERSION} --depth 1 --single-branch https://github.com/verilator/verilator.git

# Clone Icarus Verilog (built from source)
RUN cd /tmp && \
    git clone --branch ${IVERILOG_VERSION} --depth 1 --single-branch https://github.com/steveicarus/iverilog.git

ARG VERIBLE_ARCHIVE="verible-${VERIBLE_VERSION}-linux-static-x86_64.tar.gz"
RUN wget https://github.com/chipsalliance/verible/releases/download/${VERIBLE_VERSION}/${VERIBLE_ARCHIVE} -P /tmp

# Install apt package dependencies.
#
# Upgrade:
#
#   ca-certificates
#
# Efinity dependencies (as of 2025.1):
#
#   libgl1
#   libegl1
#   libxkbcommon0
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
# For UART communication with RP2040:
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
# Tools for website:
#
#   advancecomp
#

# Use unattended mode for 'apt'
ARG DEBIAN_FRONTEND="noninteractive"
RUN apt-get update && apt-get -y install --no-install-recommends \
    acme \
    advancecomp \
    autoconf \
    automake \
    binutils-multiarch \
    bison \
    build-essential \
    ca-certificates \
    cc65 \
    check \
    clang-format \
    cmake \
    flex \
    g++ \
    gcc \
    gcc-arm-none-eabi \
    gdb-multiarch \
    git-lfs \
    gperf \
    gtkwave \
    help2man \
    libegl1 \
    libfl-dev \
    libftdi-dev \
    libgl1 \
    libnewlib-arm-none-eabi \
    libstdc++-arm-none-eabi-newlib \
    libtool \
    libusb-1.0-0-dev \
    libxkbcommon0 \
    make \
    minicom \
    ninja-build \
    pkg-config \
    texinfo \
    universal-ctags

# Clean apt cache and temporary files
RUN apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Fix warnings from Cortex-Debug extension
# (See https://forums.raspberrypi.com/viewtopic.php?t=333146)
RUN cd /usr/bin && \
    ln -s /usr/bin/objdump objdump-multiarch && \
    ln -s /usr/bin/nm nm-multiarch

#
# Build and install the tools
#

# Install Verilator (built from source)
# (Do this early as it takes a while).
RUN cd /tmp/verilator && \
    autoconf && \
    ./configure && \
    make -j$(nproc) && \
    make install && \
    cd /tmp && \
    rm -rf verilator

# Install Icarus Verilog (built from source)
# (Do this early as it takes a while).
RUN cd /tmp/iverilog && \
    sh autoconf.sh && \
    sh configure && \
    make -j$(nproc) && \
    make install && \
    cd /tmp && \
    rm -rf iverilog

# Install 'picotool'
RUN cd /tmp/picotool && \
    mkdir build && \
    cd build && \
    cmake .. && \
    make -j$(nproc) && \
    make install && \
    cd /tmp && \
    rm -rf picotool

# Install OpenOCD (built from source for RP2040)
# (See: https://datasheets.raspberrypi.com/pico/getting-started-with-pico.pdf)
RUN cd /tmp/openocd && \
    ./bootstrap && \
    ./configure && \
    make -j$(nproc) && \
    sudo make install && \
    cd /tmp && \
    rm -rf openocd

# Install Efinity -- Requires a free license from Efinix. Please do the right thing.
# (See: https://www.efinixinc.com/support/)
RUN tar -xjvf /tmp/${EFINITY_ARCHIVE} -C /opt && \
    rm -rf /tmp/${EFINITY_ARCHIVE} && \
    chmod +x /opt/efinity/${EFINITY_MAJOR_MINOR}/bin/setup.sh
ENV EFINITY_HOME="/opt/efinity/${EFINITY_MAJOR_MINOR}"

# Install Verible (download from chipsalliance)
RUN mkdir -p /opt/verible && \
    tar -x -f /tmp/${VERIBLE_ARCHIVE} -C /opt/verible --strip-components 1 && \
    rm -rf /tmp/${VERIBLE_ARCHIVE}
ENV PATH="/opt/verible/bin:${PATH}"

# Install FNM (Fast Node Manager) for managing Node.js versions
# (See: https://github.com/Schniz/fnm)
ENV FNM_DIR="/opt/fnm"
RUN mkdir -p ${FNM_DIR} && \
    curl -fsSL https://fnm.vercel.app/install | bash -s -- --install-dir ${FNM_DIR} --skip-shell && \
    chmod +x ${FNM_DIR}/fnm
ENV PATH="${FNM_DIR}:$PATH"

# Install the latest Node.js LTS version
RUN eval "$(fnm env)" && \
    fnm install --lts && \
    fnm default lts-latest && \
    node --version && \
    npm --version

# Setup FNM in shell profile for root user
RUN echo 'eval "$(fnm env --use-on-cd)"' >> /root/.bashrc

# Download required ROMs (with MD5 verification)
# Note: Build context is the .devcontainer folder, so copy relative to it.
ENV ECONOPET_ROMS_DIR="/opt/econopet/roms"
COPY download-roms.sh /tmp/download-roms.sh
RUN chmod +x /tmp/download-roms.sh && \
    /tmp/download-roms.sh ${ECONOPET_ROMS_DIR} && \
    rm -f /tmp/download-roms.sh
