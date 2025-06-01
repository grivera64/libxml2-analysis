#!/usr/bin/env -S bash -le

if ! [ "$(pwd)" == "/libxml2" ]; then
    cd ./libxml2
fi

# Extract bitcode for xmlmemory
extract-bc xmlmemory.o
# extract-bc error.o
# extract-bc parser.o
# extract-bc threads.o
extract-bc testlimits.o

# llvm-link xmlmemory.o.bc error.o.bc parser.o.bc threads.o.bc testlimits.o.bc -o testlimits.bc
llvm-link xmlmemory.o.bc testlimits.o.bc -o testlimits.bc

# Convert to readable LLVM IR (just for human analysis)
llvm-dis testlimits.bc

# Generate human-readable callgraph
opt -analyze -print-callgraph testlimits.ll -o /dev/null 2> testlimits_callgraph.dot

# Analyze the bitcode using the unified pass
opt -load $UNIFIED_PATH -unified testlimits.bc -disable-output 2>&1 | tee testlimits.txt

