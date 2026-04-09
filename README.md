# VDM-RS

Vandermonde matrix-based Reed-Solomon forward error correction

## Setup

```bash
mkdir build
cmake -B build
cmake --build build
./build/unit_tests
```

<!--
brew update

# MUST READ the info that brew prints, to ensure you'll be using the brew-installed
# clang/clang++, NOT the older clang/clang++ that came with macOS.
brew install llvm clang-format make cmake

clang --version  # Should be >=22.x
cmake --version  # Should be >=4.x
gmake --version  # Should be >=4.x
-->
