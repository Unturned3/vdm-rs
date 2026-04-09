#!/usr/bin/env bash

set -euo pipefail

clang-format -i $(find include src tests -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.c' -o -name '*.cc' -o -name '*.cpp' \))
