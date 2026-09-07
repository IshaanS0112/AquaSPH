set(AQUASPH_CORE_SOURCES
    core/SPHKernel.cpp
    core/DensityPressure.cpp
    core/ForceCompute.cpp
    core/Integrator.cpp
    core/TimeStep.cpp
    core/BoundaryVolume.cpp
    core/DamBreakInit.cpp
    spatial/LinkedCell.cpp
    io/ConfigLoader.cpp
)

add_library(aquasph_core STATIC ${AQUASPH_CORE_SOURCES})

target_include_directories(aquasph_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(aquasph_core PUBLIC cxx_std_17)

target_link_libraries(aquasph_core PUBLIC
    OpenMP::OpenMP_CXX
    glm::glm
    nlohmann_json::nlohmann_json
)

add_executable(aquasph main.cpp)
target_link_libraries(aquasph PRIVATE aquasph_core)

# A second, independent executable rather than folding the renderer into
# `aquasph` -- see the comment above
# AQUASPH_BUILD_VISUALIZATION in the root CMakeLists.txt for why. Both
# executables link the same aquasph_core, so they run the identical
# physics; only main.cpp/render_main.cpp (headless reporting vs. a
# window+render loop) differ.
if(AQUASPH_BUILD_VISUALIZATION)
    add_executable(aquasph_view
        render_main.cpp
        render/GLLoader.cpp
        render/Shader.cpp
        render/Camera.cpp
        render/ParticleRenderer.cpp
    )
    target_link_libraries(aquasph_view PRIVATE aquasph_core glfw)
endif()
