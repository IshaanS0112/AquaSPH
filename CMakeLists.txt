cmake_minimum_required(VERSION 3.16)
project(AquaSPH LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# Place all executables at the build root rather than mirroring the source tree into build/src/.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

option(AQUASPH_BUILD_TESTS "Build unit tests (GoogleTest)" ON)

# Short git revision, stamped into metrics output so a recorded experiment names the code that
# produced it.
set(AQUASPH_GIT_REV "unknown")
find_package(Git QUIET)
if(AQUASPH_GIT_REV_OVERRIDE)
  set(AQUASPH_GIT_REV "${AQUASPH_GIT_REV_OVERRIDE}")
elseif(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE AQUASPH_GIT_REV
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
message(STATUS "AquaSPH revision: ${AQUASPH_GIT_REV}")
option(AQUASPH_BUILD_VISUALIZATION "Build the aquasph_view OpenGL/GLFW renderer" OFF)

# Dependencies: openMP:
# computeDensityPressure, computeForces, and the integrator's per-particle loops are
# parallelized with #pragma omp.
find_package(OpenMP REQUIRED)

# glm and nlohmann_json: prefer a system/package-manager install (find_package), fall back to
# fetching the header-only library at configure time.
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

# OpenGL/GLFW are visualization-only dependencies, used solely by the separate `aquasph_view`
# executable (src/render_main.cpp).
if(AQUASPH_BUILD_VISUALIZATION)
  # No find_package(OpenGL) here, deliberately: aquasph_view never calls a `gl*` function the
  # linker would need to resolve against -lGL.
  find_package(glfw3 3.3 QUIET)
  if(NOT glfw3_FOUND)
    message(STATUS "glfw3 not found via find_package(); fetching via FetchContent")
    # GLFW needs the X11 and GL development headers at build time (see README, Build).
    FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW TRUE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)
  endif()

  # stb_image_write: PNG encoding for --record, and nothing else.
  find_path(STB_INCLUDE_DIR stb_image_write.h)
  if(NOT STB_INCLUDE_DIR)
    message(STATUS "stb_image_write.h not found on the system; fetching stb via FetchContent")
    FetchContent_Declare(stb
      GIT_REPOSITORY https://github.com/nothings/stb.git
      GIT_TAG f0569113c93ad095470c54bf34a17b36646bbbb5
      GIT_SHALLOW FALSE)
    FetchContent_MakeAvailable(stb)
    set(STB_INCLUDE_DIR ${stb_SOURCE_DIR})
  endif()
endif()

add_subdirectory(src)

if(AQUASPH_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Copy configs/ next to the built executable so `cd build && ./aquasph` resolves
# "configs/default.json" without extra flags, matching the documented build/run instructions in
# README.md.
file(COPY ${CMAKE_SOURCE_DIR}/configs DESTINATION ${CMAKE_BINARY_DIR})
