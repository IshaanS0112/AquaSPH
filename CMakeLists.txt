cmake_minimum_required(VERSION 3.16)
project(AquaSPH LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

option(AQUASPH_BUILD_TESTS "Build unit tests (GoogleTest)" ON)
option(AQUASPH_BUILD_VISUALIZATION "Build the aquasph_view OpenGL/GLFW renderer (Phase 1.5)" OFF)

# --- Dependencies ---------------------------------------------------------
# OpenMP: linked from Phase 0 onward, and actually used starting Phase 1 --
# computeDensityPressure, computeForces, and the integrator's per-particle
# loops are parallelized with #pragma omp. LinkedCell::build is
# deliberately still serial (see spatial/LinkedCell.cpp for why).
find_package(OpenMP REQUIRED)

# glm and nlohmann_json: prefer a system/package-manager install
# (find_package), fall back to fetching the header-only library at
# configure time. This keeps the build working both on a dev machine with
# apt/vcpkg-installed deps and on a bare sandbox with neither.
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

# OpenGL/GLFW are Phase 1.5 (visualization) dependencies, used only by
# the separate `aquasph_view` executable (src/render_main.cpp) -- the
# headless `aquasph` binary that Phase 0/1 and every test target depend
# on never links either, so a machine without GL dev headers (e.g. a
# minimal CI runner, or the sandbox this project was largely developed
# in -- see docs/architecture.md, "Phase 1.5") can still build and test
# everything else with AQUASPH_BUILD_VISUALIZATION left at its default
# OFF. Flip it ON on a real desktop Linux/macOS/Windows machine to build
# the viewer too.
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
    # (verified empirically in this project's own dev sandbox: the
    # runtime .so files were present but every one of these dev headers
    # was missing, which is exactly what makes GLFW_BUILD_X11 fail to
    # configure -- see docs/architecture.md, "Phase 1.5", for the full
    # investigation.) macOS/Windows need no extra system packages beyond
    # a normal Xcode/MSVC toolchain.
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
