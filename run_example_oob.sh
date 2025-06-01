#!/usr/bin/env -S bash -le

if ! [ "$(pwd)" == "/libxml2" ]; then
    cd ./libxml2
fi

export LD_LIBRARY_PATH="/usr/local/lib/:$LD_LIBRARY_PATH"
clang -o example_oob -I. -Iinclude/ example_oob.c -L.libs/ -lxml2 -lpthread
./example_oob

