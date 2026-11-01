# NumKit

[![License: 0BSD](https://img.shields.io/badge/License-0BSD-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B17)

**NumKit** is a lightweight, high-performance, MATLAB-compatible runtime, Ahead-of-Time (AOT) C++17 transpiler, and scientific computing ecosystem written in modern C++17.

It features a dual-engine execution core (TreeWalker AST interpreter + Bytecode Stack VM), incremental reactive evaluation (`ReactiveSession`), AOT compilation to native binaries via `numkit-codegen` (powered by a portable `zig c++` toolchain), Highway SIMD vectorization, and dynamic toolbox loading.

---

## Quick Install (From Source)

`numkit/install` provisions a self-contained build environment in `~/.numkit/` (automatically fetching a portable **Zig C++17 toolchain**, **CMake**, and **Ninja** when needed) and compiles `numkit` and `numkit-codegen` natively for your CPU.

### Windows (PowerShell)
```powershell
irm https://raw.githubusercontent.com/numkit/install/main/install.ps1 | iex
```

### Linux & macOS
```bash
curl -fsSL https://raw.githubusercontent.com/numkit/install/main/install.sh | sh
```

---

## Usage

### Interactive REPL & Script Execution
```bash
# Start interactive REPL
numkit

# Run a .m script
numkit script.m

# Evaluate a one-liner
numkit -e "disp(sum(1:100));"
```

### AOT Compilation & Fast Native Execution (`numkit-codegen`)
```bash
# Transpile a .m script/function to C++17, compile via zig c++, and run immediately
numkit --aot script.m

# Compile a .m script/function into a standalone native executable
numkit build script.m -o app.exe
./app.exe

# Emit standalone C++17 source code
numkit codegen script.m -o script.cpp
```

### Package & Toolbox Management
```bash
# Install an official or third-party toolbox from source
numkit pkg install signal
numkit pkg install stats

# List installed toolboxes
numkit pkg list

# Remove an installed toolbox
numkit pkg remove signal
```

---

## Ecosystem Architecture

NumKit is organized as a modular ecosystem under [`github.com/numkit`](https://github.com/numkit):

| Repository | Layer | Description |
|---|---|---|
| **[numkit/value](https://github.com/numkit/value)** | Foundation (L0) | N-dimensional array container (`Value`), PMR memory arenas, type traits, `argcheck` |
| **[numkit/sys](https://github.com/numkit/sys)** | Foundation (L0) | Virtual filesystem (`FsContext`), OS process runner, toolchain discovery (`zig c++`, `msvc`, `clang`, `gcc`, `cmake`, `ninja`), and C-ABI plugin interface |
| **[numkit/ops](https://github.com/numkit/ops)** | Compute (L1) | Engine-free SIMD kernels (Google Highway), BLAS 1/2/3, LAPACK decompositions, FFT, ODE/optimization solvers, RNG, and image I/O |
| **[numkit/numkit](https://github.com/numkit/numkit)** | Core Engine (L2) | Lexer, Parser, AST, TreeWalker, Bytecode VM, `ReactiveSession`, built-in language library, `pkg` manager, and `numkit` CLI/REPL |
| **[numkit/codegen](https://github.com/numkit/codegen)** | AOT Transpiler (L2) | Ahead-of-time C++17 transpiler (`numkit-codegen`), static type inference, and C-ABI runtime bridge |
| **[numkit/signal](https://github.com/numkit/signal)** | Toolbox Plugin | Digital Signal Processing (filtering, spectral analysis, multirate, windows, transforms) |
| **[numkit/control](https://github.com/numkit/control)** | Toolbox Plugin | Control Systems (`tf`, `ss`, `zpk`, LQR/Kalman, frequency/time response) |
| **[numkit/stats](https://github.com/numkit/stats)** | Toolbox Plugin | Statistics & Machine Learning (distributions, hypothesis tests, regression, clustering) |
| **[numkit/wavelet](https://github.com/numkit/wavelet)** | Toolbox Plugin | Wavelet transforms (DWT, MODWT, CWT, wavelet packets, denoising) |
| **[numkit/audio](https://github.com/numkit/audio)** | Toolbox Plugin | Audio processing (MFCC, pitch/spectral descriptors, perceptual weighting) |
| **[numkit/comm](https://github.com/numkit/comm)** | Toolbox Plugin | Communications (modulation, channel coding, Galois fields, channels) |
| **[numkit/image](https://github.com/numkit/image)** | Toolbox Plugin | Image processing (filtering, morphology, geometry, color spaces, segmentation) |
| **[numkit/install](https://github.com/numkit/install)** | Bootstrapper | One-line cross-platform source installer (`install.ps1` / `install.sh`) |
| **[numkit/bundle](https://github.com/numkit/bundle)** | Superbuild & QA | Umbrella superbuild, parity suite, differential fieldtest harness, and architectural audits |

---

## Manual CMake Build

```bash
git clone https://github.com/numkit/numkit.git
cd numkit
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

---

## License

NumKit is released under the **[BSD Zero Clause License (0BSD)](LICENSE)**.
