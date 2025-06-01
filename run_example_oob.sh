#!/usr/bin/env -S bash -le

if ! [ "$(pwd)" == "/libxml2" ]; then
    cd ./libxml2
fi

export LD_LIBRARY_PATH="/usr/local/lib/:$LD_LIBRARY_PATH"
clang -o example_oob `xml2-config --cflags` example_oob.c `xml2-config --libs` -lpthread
./example_oob

