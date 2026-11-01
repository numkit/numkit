# NumKit

[![License: 0BSD](https://img.shields.io/badge/License-0BSD-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B17)

**NumKit** is a lightweight, high-performance, MATLAB-compatible runtime and computational environment written in modern C++17.

It features a dual-engine architecture (TreeWalker AST interpreter + Bytecode Stack VM), dynamic module loading, Highway SIMD vectorization, and zero-dependency MAT-file I/O.

---

## Architecture

NumKit is organized as a modular ecosystem:

- **[numkit/numkit](https://github.com/numkit/numkit)**: Core runtime, parser, TreeWalker + Bytecode VM, built-in language library, package manager, and interactive REPL.
- **[numkit/value](https://github.com/numkit/value)**: Foundational value representation (`Value`), memory resources, and plugin C-ABI.
- **[numkit/sys](https://github.com/numkit/sys)**: Virtual filesystem (`FsContext`), OS process execution, environment, and toolchain discovery.
- **[numkit/ops](https://github.com/numkit/ops)**: Low-level numerical kernels (Highway SIMD, BLAS 1/2/3, LAPACK decompositions, FFT, ODE/optimization solvers, random number generators).
- **[numkit/codegen](https://github.com/numkit/codegen)**: Ahead-of-time (AOT) C++17 transpiler, static type-inference engine, and runtime C-ABI bridge.
- **Domain Toolboxes**:
  - **[numkit/signal](https://github.com/numkit/signal)**: Digital Signal Processing (filtering, spectral analysis, multirate, windows).
  - **[numkit/control](https://github.com/numkit/control)**: Control Systems (`tf`, `ss`, `zpk`, LQR/Kalman, frequency/time response).
  - **[numkit/stats](https://github.com/numkit/stats)**: Statistics & Machine Learning (distributions, hypothesis tests, regression, clustering).
  - **[numkit/wavelet](https://github.com/numkit/wavelet)**: Wavelet transforms (DWT, MODWT, CWT, wavelet packets, denoising).
  - **[numkit/audio](https://github.com/numkit/audio)**: Audio processing (MFCC, pitch/spectral descriptors, perceptual weighting).
  - **[numkit/comm](https://github.com/numkit/comm)**: Communications (modulation, channel coding, Galois fields, channels).
  - **[numkit/image](https://github.com/numkit/image)**: Image processing (filtering, morphology, geometry, color spaces, segmentation).
- **[numkit/registry](https://github.com/numkit/registry)**: Central index and registry for packages and toolboxes.

---

## Building

### Prerequisites

- CMake 3.21+
- C++17 compliant compiler (MSVC 2019+, GCC 9+, Clang 10+)
- Git

### Build Instructions

```bash
git clone https://github.com/numkit/numkit.git
cd numkit
cmake -B build -S .
cmake --build build --config Release
```

The build produces:
- `numkit` (or `numkit.exe` on Windows): Interactive REPL and script runner.
- `libnumkit.a` (or `numkit.lib`): Static library for embedding.
- `numkit_gtest`: Core test suite.

---

## Usage

### Interactive REPL

```bash
./build/Release/numkit
```

```matlab
>> A = [1, 2; 3, 4]
A =
     1     2
     3     4

>> b = [5; 6]
b =
     5
     6

>> x = 1:10;
>> sum(x .^ 2)
ans =
   385
```

### Script Execution

```bash
./build/Release/numkit myscript.m
```

### One-line Evaluation

```bash
./build/Release/numkit -e "disp(sum(1:100));"
```

### Loading Toolboxes (Plugins)

Toolboxes are compiled dynamic libraries (`.dll` / `.so`). They can be loaded at runtime via `loadmodule`:

```matlab
loadmodule('path/to/numkit_signal.dll');
[b, a] = butter(4, 0.2);
```

---

## License

NumKit is released under the **[BSD Zero Clause License (0BSD)](LICENSE)**.
