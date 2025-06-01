#!/usr/bin/env -S bash -le

if ! [ "$(pwd)" == "/libxml2" ]; then
    cd ./libxml2
fi

# Extract bitcode for xmlmemory
extract-bc xmlmemory.o

# Compile our example_oob.c main file
wllvm -c -I. -Iinclude/ example_oob.c
extract-bc example_oob.o

llvm-link xmlmemory.o.bc example_oob.o.bc -o example_oob.bc

# Convert to readable LLVM IR (just for human analysis)
llvm-dis example_oob.bc

# Generate human-readable callgraph
opt -analyze -print-callgraph example_oob.bc -o /dev/null 2> example_oob_callgraph.txt

# Analyze the bitcode using the unified pass
opt -load $UNIFIED_PATH -unified example_oob.bc -disable-output 2>&1 | tee example_oob.txt

