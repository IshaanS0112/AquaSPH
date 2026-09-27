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
    test_forces.cpp
    test_timestep.cpp
    test_determinism.cpp
    test_boundary.cpp
    test_scene.cpp
    test_dynamic_particles.cpp
    test_extensibility.cpp
    test_json.cpp
)

add_executable(aquasph_tests ${AQUASPH_TEST_SOURCES})
target_link_libraries(aquasph_tests PRIVATE aquasph_core gtest gtest_main)
target_include_directories(aquasph_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

include(GoogleTest)
gtest_discover_tests(aquasph_tests)

# Process-contract tests: exit codes, signal handling and the
# --progress-json stream, exercised on the real binary. The platform
# worker in backend/ is built on exactly these promises.
if(UNIX)
  foreach(case progress_stream sigterm_cancels profile_conflict load_error_exit_code)
    add_test(NAME CliContract.${case}
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli/test_cli_contract.sh
                     $<TARGET_FILE:aquasph>
                     ${CMAKE_SOURCE_DIR}/configs/scenarios/dam_break.json
                     ${case})
    set_tests_properties(CliContract.${case} PROPERTIES TIMEOUT 120)
  endforeach()
endif()
