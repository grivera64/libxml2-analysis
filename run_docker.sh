#!/usr/bin/env bash

docker build -t "libxml2-analysis" .
docker run -it --name "libxml2-analysis" "libxml2-analysis" /bin/bash
docker rm "libxml2-analysis"
