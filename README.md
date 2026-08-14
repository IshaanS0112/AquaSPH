<div align="center">

# AquaSPH

**A 3D Smoothed Particle Hydrodynamics fluid simulation engine in C++17**

[![CI](https://github.com/IshaanS0112/AquaSPH/actions/workflows/ci.yml/badge.svg)](https://github.com/IshaanS0112/AquaSPH/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C.svg?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/17)
[![CMake](https://img.shields.io/badge/CMake-3.16%2B-064F8C.svg?logo=cmake&logoColor=white)](https://cmake.org/)
[![OpenMP](https://img.shields.io/badge/OpenMP-parallel-EE4C2C.svg)](https://www.openmp.org/)
[![OpenGL](https://img.shields.io/badge/OpenGL-3.3%20core-5586A4.svg?logo=opengl)](https://www.opengl.org/)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey.svg)](#build)
[![Tests](https://img.shields.io/badge/tests-20%20passing-brightgreen.svg)](#testing)

Lagrangian fluid solver with linked-cell neighbor search, OpenMP-parallelized
physics, and a real-time OpenGL viewer.

</div>

---

## Overview

SPH is a Lagrangian, meshless method for simulating fluids: rather than
discretizing space onto a fixed grid, the fluid is represented as particles
carrying their own mass, density, and pressure, interacting with neighbors
through a smoothing kernel. It underpins production VFX fluid tools and is
standard in computational-physics and CFD coursework.

AquaSPH implements the full pipeline from first principles — kernel
interpolation, density estimation, equation of state, force computation, and
time integration — with an emphasis on numerical correctness and measured
performance rather than visual plausibility alone.

## Results

Dam-break scenario, Release build, 4-core machine. All runs stable
(zero NaN, zero out-of-domain particles):

| Particles | 1 thread | 2 threads | 4 threads | Speedup (4T) | Efficiency |
|----------:|---------:|----------:|----------:|-------------:|-----------:|
| 5,832  | 136.4 FPS | 252.6 FPS | 476.7 FPS | 3.49× | 87% |
| 10,648 | 34.3 FPS  | 65.0 FPS  | 118.5 FPS | 3.46× | 86% |
| 27,000 | 5.3 FPS   | 10.2 FPS  | 16.9 FPS  | 3.17× | 79% |
| 50,653 | 1.4 FPS   | 2.6 FPS   | 4.1 FPS   | 3.03× | 76% |

Parallel output is **bit-identical** across thread counts — verified by
comparing density, position, and velocity statistics at 1, 2, and 4 threads
across a 250-step run. Full methodology, the 8-thread oversubscription
result, and an analysis of the declining efficiency at scale are in
[`benchmarks/scaling_results.md`](benchmarks/scaling_results.md).

## Features

**Core solver**

- Cubic spline (M4) smoothing kernel with analytic gradient and 3D-correct normalization
- Linked-cell spatial hashing — O(N) neighbor search via a 27-cell stencil, vs. O(N²) all-pairs
- SPH density estimation with Tait equation of state (weakly-compressible water)
- Force computation: symmetric pressure gradient, SPH-discretized viscosity, gravity
- Second-order predictor–corrector time integration
- JSON-configurable simulation parameters with safe fallback to defaults
- Dam-break scenario stable for 1,200+ timesteps at 8,000 particles

**Parallelism**

- Density, force, and integrator loops parallelized over particles with OpenMP
- Per-thread neighbor buffers eliminate the data race a naive parallelization would introduce
- Runtime thread control via `--threads N`
- Determinism preserved: thread count affects wall-clock time, not results

**Visualization**

- Optional real-time viewer (`aquasph_view`) built on OpenGL 3.3 core + GLFW
- Particles rendered as circular point sprites, colored by speed
- Mouse-orbit camera with scroll zoom
- Minimal hand-written GL function loader — no GLAD/GLEW dependency
- Shares the solver and initializer with the headless binary, so the rendered
  simulation is provably identical to the benchmarked one

**Quality**

- 20 GoogleTest unit tests covering kernel, EOS, neighbor search, and integrator
- Continuous integration on Ubuntu and macOS: build, full test suite, and a
  headless simulation smoke run on every push
- Clean build under `-Wall -Wextra`

## Build

Requires CMake 3.16+, a C++17 compiler, and OpenMP. `glm`, `nlohmann_json`,
and GoogleTest resolve via `find_package` if installed, and are fetched
automatically through CMake `FetchContent` otherwise — no vendored sources.

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j4
```

<details>
<summary><b>macOS — additional OpenMP configuration</b></summary>

Apple Clang does not bundle OpenMP. Homebrew's `libomp` is keg-only, so it is
not placed on the default search path and CMake's `FindOpenMP` cannot locate
its flags and library from an `OpenMP_ROOT` hint alone. Pass the cache
variables explicitly:

```bash
brew install libomp
cmake -DCMAKE_BUILD_TYPE=Release \
  -DOpenMP_CXX_FLAGS="-Xpreprocessor -fopenmp -I$(brew --prefix libomp)/include" \
  -DOpenMP_CXX_LIB_NAMES="omp" \
  -DOpenMP_omp_LIBRARY="$(brew --prefix libomp)/lib/libomp.dylib" \
  ..
make -j4
```

Verified on Apple Silicon with AppleClang 21 and CMake 4.4.2.

</details>

## Usage

```bash
./aquasph                                   # default scenario (configs/default.json)
./aquasph --particles 20000 --steps 500     # override particle and step count
./aquasph --threads 4                       # override OpenMP thread count
./aquasph --config ../configs/default.json  # explicit config path
./aquasph --quiet                           # suppress periodic progress output
```

Periodic output reports step timing, throughput, min/avg/max SPH density, and
a count of non-finite or out-of-domain particles. The run terminates with a
`STABLE` / `UNSTABLE` verdict and a matching process exit code, making it
directly usable as a CI check.

Thread counts above the machine's physical core count generally reduce
throughput — see [`benchmarks/scaling_results.md`](benchmarks/scaling_results.md)
for measurements.

## Visualization

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DAQUASPH_BUILD_VISUALIZATION=ON ..
make -j4
./aquasph_view                          # default scenario
./aquasph_view --particles 20000        # denser particle block
```

Left-drag orbits the camera, scroll zooms, `Esc` exits. Particles are colored
by speed — deep blue at rest, white at maximum velocity, making the initial
floor impact and residual turbulence immediately visible.

The viewer is opt-in so that the headless solver and the entire test suite
build on machines without GL development headers. Platform prerequisites are
documented in [`docs/architecture.md`](docs/architecture.md).

## Testing

```bash
cd build
ctest --output-on-failure
```

20 tests covering kernel normalization (validated against numerical
integration), gradient symmetry and sign, Tait EOS behavior under compression
and expansion, linked-cell neighbor correctness for interior and boundary
cells, and integrator physics (parabolic free-fall, wall containment, bounded
energy under repeated steps).

## Project structure

```
src/core/            Particle model, SPH kernel, density/EOS, forces, integrator, scenario init
src/spatial/         Linked-cell neighbor search
src/io/              JSON configuration loading
src/benchmark/       Timing instrumentation
src/render/          GL loader, shader, camera, particle renderer
src/main.cpp         Headless solver and benchmark entry point
src/render_main.cpp  Real-time viewer entry point
configs/             Simulation parameters (JSON)
tests/               GoogleTest unit tests
benchmarks/          Measured throughput and scaling results
docs/                Architecture and design-decision documentation
```

## Documentation

[`docs/architecture.md`](docs/architecture.md) covers the module breakdown and
data flow, the parallelization strategy and why one component remains serial,
the AoS/SoA tradeoff, and — most substantially — a detailed account of five
correctness problems resolved during development:

1. A kernel normalization constant that was dimensionally 2D, applied in 3D
2. A discontinuous piecewise kernel polynomial, caught by a monotonicity test
3. A dimensionally ill-defined viscosity term, replaced with the standard
   SPH velocity-Laplacian discretization
4. A CFL violation of roughly 35× between the sound speed and timestep
5. Two runtime instabilities — free-surface tensile instability and a
   floor-impact force spike — diagnosed through instrumentation

## Roadmap

- CUDA acceleration of the density and force kernels
- Adaptive timestepping driven by a CFL and max-force criterion
- Boundary force particles to replace the current clamp-and-damp wall model
- Structure-of-Arrays particle layout, pending profiler confirmation

## License

Released under the [MIT License](LICENSE).
