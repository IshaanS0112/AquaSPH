set(AQUASPH_CORE_SOURCES
    core/SPHKernel.cpp
    core/DensityPressure.cpp
    core/ForceCompute.cpp
    core/BoundaryVolume.cpp
    core/Integrator.cpp
    core/TimeStep.cpp
    spatial/LinkedCell.cpp
    scene/Shapes.cpp
    scene/TimeSeries.cpp
    scene/Scenario.cpp
    scene/ScenarioLoader.cpp
    scene/Simulation.cpp
    metrics/Metrics.cpp
)

add_library(aquasph_core STATIC ${AQUASPH_CORE_SOURCES})

target_include_directories(aquasph_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(aquasph_core PUBLIC cxx_std_17)

# Stamped into every metrics file so a recorded result can be tied back to the exact code that
# produced it (docs/experiments.md).
target_compile_definitions(aquasph_core PUBLIC AQUASPH_GIT_REV="${AQUASPH_GIT_REV}")

target_link_libraries(aquasph_core PUBLIC
    OpenMP::OpenMP_CXX
    glm::glm
    nlohmann_json::nlohmann_json
)

add_executable(aquasph main.cpp)
target_link_libraries(aquasph PRIVATE aquasph_core)

# A second, independent executable rather than folding the renderer into `aquasph`.
if(AQUASPH_BUILD_VISUALIZATION)
    add_executable(aquasph_view
        render_main.cpp
        render/GLLoader.cpp
        render/Shader.cpp
        render/Camera.cpp
        render/ParticleRenderer.cpp
        render/FluidRenderer.cpp
        render/Environment.cpp
        render/FrameCapture.cpp
        render/OffscreenTarget.cpp
    )
    target_link_libraries(aquasph_view PRIVATE aquasph_core glfw)
    # SYSTEM: stb_image_write.h is third-party and does not build clean under -Wall -Wextra.
    target_include_directories(aquasph_view SYSTEM PRIVATE ${STB_INCLUDE_DIR})
endif()
