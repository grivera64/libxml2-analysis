#!/usr/bin/env -S bash -le
if ! [ "$(pwd)" == "/libxml2" ]; then
    cd ./libxml2
fi

# Generate configure files
./autogen.sh

# Configure libxml2 build system
./configure CFLAGS='-g -O0 -DDEBUG_MEMORY'

# Build the library
make V=1
