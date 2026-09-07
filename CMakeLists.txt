cmake_minimum_required(VERSION 3.16)
project(AquaSPH LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# Place all executables at the build root rather than mirroring the source
# tree into build/src/. Without this, `aquasph` lands in build/src/ while
# the configs/ directory copied below sits in build/ -- so the documented
# `cd build && ./aquasph` would resolve neither the binary nor its default
# config path. Keeping both at the build root makes the documented commands
# correct and keeps the CI invocation identical to the local one.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

option(AQUASPH_BUILD_TESTS "Build unit tests (GoogleTest)" ON)

# Short git revision, stamped into metrics output so a recorded experiment
# names the code that produced it. Best-effort: a source tarball with no
# .git directory still configures and builds, it just reports "unknown".
set(AQUASPH_GIT_REV "unknown")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE AQUASPH_GIT_REV
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
message(STATUS "AquaSPH revision: ${AQUASPH_GIT_REV}")
option(AQUASPH_BUILD_VISUALIZATION "Build the aquasph_view OpenGL/GLFW renderer" OFF)

# --- Dependencies ---------------------------------------------------------
# OpenMP: computeDensityPressure, computeForces, and the integrator's
# per-particle loops are parallelized with #pragma omp. LinkedCell::build
# is deliberately still serial (see spatial/LinkedCell.cpp for why).
find_package(OpenMP REQUIRED)

# glm and nlohmann_json: prefer a system/package-manager install
# (find_package), fall back to fetching the header-only library at
# configure time. This keeps the build working both on a dev machine with
# apt/vcpkg-installed deps and on a bare machine with neither.
include(FetchContent)

find_package(glm QUIET)
if(NOT glm_FOUND)
  message(STATUS "glm not found via find_package(); fetching via FetchContent")
  FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(glm)
endif()

find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
  message(STATUS "nlohmann_json not found via find_package(); fetching via FetchContent")
  FetchContent_Declare(json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(json)
endif()

# OpenGL/GLFW are visualization-only dependencies, used solely by the
# separate `aquasph_view` executable (src/render_main.cpp). The headless
# `aquasph` binary and every test target link neither, so a machine
# without GL development headers (a minimal CI runner, for example) can
# still build and test everything else with AQUASPH_BUILD_VISUALIZATION
# left at its default OFF. Enable it on a desktop Linux/macOS/Windows
# machine to build the viewer as well.
if(AQUASPH_BUILD_VISUALIZATION)
  # No find_package(OpenGL) here, deliberately: aquasph_view never calls
  # a `gl*` function the linker would need to resolve against -lGL --
  # every GL call goes through the function pointers in
  # src/render/GLLoader.cpp, resolved at *runtime* via
  # glfwGetProcAddress. GLFW itself works the same way (it dlopens
  # libGL.so and resolves GLX entry points at runtime rather than
  # linking them at build time -- confirmed directly: GLFW's own
  # CMakeLists.txt configures its X11 backend with no GL/gl.h or
  # GL/glx.h anywhere in sight; the actual blockers turned out to be the
  # X11 *extension* headers below), so nothing in this project needs to
  # find or link OpenGL as a CMake package at all.
  find_package(glfw3 3.3 QUIET)
  if(NOT glfw3_FOUND)
    message(STATUS "glfw3 not found via find_package(); fetching via FetchContent")
    # GLFW's own CMake requires the X11 *development* headers at
    # configure/build time (not just the runtime .so files) for its X11
    # backend: mesa-common-dev + libgl1-mesa-dev (GL/gl.h, GL/glx.h) and
    # libxrandr-dev, libxinerama-dev, libxcursor-dev, libxi-dev,
    # libx11-dev (their respective X11/extensions/*.h). On Debian/Ubuntu:
    #   sudo apt install libglfw3-dev mesa-common-dev libgl1-mesa-dev \
    #     libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libx11-dev
    # A system carrying the runtime .so files but not these -dev packages
    # is the most common cause of GLFW's configure failing with
    # "RandR headers not found". macOS/Windows need no extra system
    # packages beyond a normal Xcode/MSVC toolchain.
    FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW TRUE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)
  endif()
endif()

add_subdirectory(src)

if(AQUASPH_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Copy configs/ next to the built executable so `cd build && ./aquasph`
# resolves "configs/default.json" without extra flags, matching the
# documented build/run instructions in README.md.
file(COPY ${CMAKE_SOURCE_DIR}/configs DESTINATION ${CMAKE_BINARY_DIR})
