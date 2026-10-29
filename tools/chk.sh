#!/bin/bash
export DEVKITPRO=/opt/devkitpro
export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
cd "$(dirname "$0")/../src/gen"
aarch64-none-elf-g++ -std=gnu++17 -fsyntax-only -fpermissive -w "$@" game_gen.cpp 2>&1
