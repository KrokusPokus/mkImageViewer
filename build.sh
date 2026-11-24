#!/bin/bash

if [ ! -d "build" ]; then
    mkdir build
fi

cd build
cmake -DCMAKE_BUILD_TYPE=Release -DMK_NATIVE=ON ..
cmake --build . --parallel
