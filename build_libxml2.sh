#!/usr/bin/env -S bash -le
if ! [ "$(pwd)" == "/libxml2" ]; then
    cd ./libxml2
fi

# Generate configure files
./autogen.sh

# Configure libxml2 build system
./configure
rg "\-g \-O2" --files-with-matches --glob '!build_libxml2.sh' | xargs sed -i 's/\-g \-O2/\-DDEBUG_MEMORY_LOCATION \-g \-O0/g'

# Build the library
make clean
make V=1

