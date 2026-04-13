# VDM-RS

Vandermonde matrix-based Reed-Solomon forward error correction library

Read `plan.md` for what's left to implement.

See [piazza](https://piazza.com/class/mjzz4wzsihz3kg/post/86) for grading criteria.


## Environment Setup

This repo contains git submodules. Clone using:
```bash
git clone --recurse-submodules <repo_url>
```

Do __NOT__ install the C/C++ vscode extension from Microsoft (`ms-vscode.cpptools`).

Install the `clangd` extension instead (`llvm-vs-code-extensions.vscode-clangd`).

```bash
brew update

# MUST READ the info that brew prints, to ensure you'll be using the
# brew-installed tools, NOT the older tools that came with macOS.
brew install llvm clang-format make cmake

clang --version  # Should be >=22.x
cmake --version  # Should be >=4.x
gmake --version  # Should be >=4.x
```

## Build

```bash
mkdir build
cmake -B build
cmake --build build
./build/unit_tests
```


## Misc

- Set `editor.formatOnSave` to `true` in vscode settings
- Run `./format-code.sh` prior to commits
