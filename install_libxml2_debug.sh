#!/usr/bin/env -S bash -e

if ! [ "$(pwd)" == "/libxml2" ]; then
    cd ./libxml2
fi

# Generate configure files
CC=clang ./autogen.sh

# Configure libxml2 build system
CC=clang ./configure CFLAGS='-g -O0 -DDEBUG_MEMORY'

# Build the library
CC=clang make V=1 -j$(nproc) install
