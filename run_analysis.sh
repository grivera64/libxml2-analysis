#!/usr/bin/env -S bash -le

if ! [ "$(pwd)" == "/libxml2" ]; then
    cd ./libxml2
fi

# Extract bitcode for xmlmemory
extract-bc xmlmemory.o

# Convert to readable LLVM IR (just for human analysis)
llvm-dis xmlmemory.o.bc

# Analyze the bitcode using the unified pass
opt -load $UNIFIED_PATH -unified xmlmemory.o.bc -disable-output &2>&1 | tee xmlmemory.txt

