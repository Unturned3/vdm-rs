# VDM-RS

Vandermonde matrix-based Reed-Solomon forward error correction

## Setup

Assuming you're using macOS:

```
brew install llvm clang-format gmake cmake
export PATH="/usr/local/opt/llvm/bin:$PATH"
clang --version  # Should be >=22.x
cmake --version  # Should be >=4.x
gmake --version  # Should be >=4.x

CC=$(which clang) CXX=$(which clang++) cmake -B build
cd build
gmake
./main 1 2  # Should print "Host: 1, Port: 2"
```
