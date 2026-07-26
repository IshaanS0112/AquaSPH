include(FetchContent)

find_package(GTest QUIET)
if(NOT GTest_FOUND)
  message(STATUS "GTest not found via find_package(); fetching via FetchContent")
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
    GIT_SHALLOW TRUE
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

set(AQUASPH_TEST_SOURCES
    test_kernel.cpp
    test_density.cpp
    test_neighbor.cpp
    test_integrator.cpp
)

add_executable(aquasph_tests ${AQUASPH_TEST_SOURCES})
target_link_libraries(aquasph_tests PRIVATE aquasph_core gtest gtest_main)
target_include_directories(aquasph_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

include(GoogleTest)
gtest_discover_tests(aquasph_tests)
