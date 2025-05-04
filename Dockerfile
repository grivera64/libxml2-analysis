# libxml2 analysis Dockerfile
# with Unified LLVM-10 Pass
# (From https://github.com/Lightninghkm/Unified-Memory-Safety-Validation)
#
# Run with:
# ---------------------------
# git clone git@github.com:grivera64/libxml2-analysis.git
# docker build -t "libxml2-analysis" .
# docker run -it --name "libxml2-analysis" "libxml2-analysis" /bin/bash
# 
# And stop running by running
# ---------------------------
# docker kill "libxml2-analysis"
# docker rm "libxml2-analysis"
#
FROM ubuntu:20.04

ENV LLVM_DIR=/usr/lib/llvm-10/

WORKDIR /GitHub

# Needed for apt-get
ENV TZ=America/Los_Angeles
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

RUN apt-get update && apt-get install -y \
    git \
    cmake \
    ninja-build \
    build-essential \
    python3-minimal python3-pip \
    wget \
    libzstd-dev \
    software-properties-common \
    vim \
    autoconf libtool automake \
    pkg-config zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Install LLVM-10
RUN wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | apt-key add - \
    && apt-add-repository "deb http://apt.llvm.org/focal/ llvm-toolchain-focal-10 main"
RUN apt-get update && apt-get install -y \
    llvm-10 \
    llvm-10-dev \
    llvm-10-tools \
    clang-10 \
    && rm -rf /var/lib/apt/lists/*

# Alias 10 version to normal
RUN ln -sfn /usr/bin/opt-10 /usr/bin/opt \
    && ln -sfn /usr/bin/clang-10 /usr/bin/clang \
    && ln -sfn /usr/bin/llvm-dis-10 /usr/bin/llvm-dis \
    && ln -sfn /usr/bin/llvm-as-10 /usr/bin/llvm-as \
    && ln -sfn /usr/bin/lli-10 /usr/bin/lli \
    && ln -sfn /usr/bin/llc-10 /usr/bin/llc \
    && ln -sfn /usr/bin/llvm-link-10 /usr/bin/llvm-link

# Clone the repository
RUN git clone https://github.com/Lightninghkm/Unified-Memory-Safety-Validation.git

# Build SVF
WORKDIR /GitHub/Unified-Memory-Safety-Validation/program-dependence-graph/SVF 
RUN ./build.sh

# Build PDG
WORKDIR /GitHub/Unified-Memory-Safety-Validation/program-dependence-graph/build
RUN cmake .. \
    && make

# Build libUnifiedMemSafe.so (Validation)
WORKDIR /GitHub/Unified-Memory-Safety-Validation
RUN cmake . \
    && make

RUN cp libUnifiedMemSafe.so $LLVM_DIR/lib/

ENV UNIFIED_PATH=$LLVM_DIR/lib/libUnifiedMemSafe.so

WORKDIR /libxml2
COPY . .
RUN ./configure

# Run in background
CMD ["/bin/bash", "-c", "while true; do sleep 60; done"]
