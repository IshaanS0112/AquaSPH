// AquaSPH -- real-time OpenGL viewer.
//
// A separate executable from `aquasph` (main.cpp) on purpose: the
// headless benchmark/CI binary has no reason to link GLFW/OpenGL, and
// this file has no reason to carry the benchmark-reporting logic. Both
// share the exact same physics call sequence (grid.build ->
// computeDensityPressure -> computeForces -> integrator.step) and the
// exact same dam-break initializer (core/DamBreakInit.*) as main.cpp, so
// what's on screen here is provably the same simulation `aquasph`
// benchmarks, not a separate/simplified copy that could quietly drift.
//
// Requires a real display and OpenGL 3.3+ drivers; see
// docs/architecture.md, "Platform requirements for the viewer", for
// per-platform prerequisites.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <iostream>
#include <string>
#include <vector>

#include "core/Particle.hpp"
#include "core/DamBreakInit.hpp"
#include "core/SPHKernel.hpp"
#include "core/DensityPressure.hpp"
#include "core/ForceCompute.hpp"
#include "core/Integrator.hpp"
#include "spatial/LinkedCell.hpp"
#include "io/ConfigLoader.hpp"
#include "render/GLLoader.hpp"
#include "render/Camera.hpp"
#include "render/ParticleRenderer.hpp"

using namespace aquasph;

namespace {

struct InputState {
    bool dragging = false;
    double lastX = 0.0;
    double lastY = 0.0;
    OrbitCamera* camera = nullptr;
};

void cursorPosCallback(GLFWwindow* window, double xpos, double ypos) {
    auto* input = static_cast<InputState*>(glfwGetWindowUserPointer(window));
    if (input->dragging) {
        const float dx = static_cast<float>(xpos - input->lastX);
        const float dy = static_cast<float>(ypos - input->lastY);
        // Sign convention: dragging right moves the view "the same way
        // a trackball would" (orbit follows the cursor), dragging up
        // looks up from below rather than tipping over -- hence the
        // negated dy.
        input->camera->orbit(dx * 0.3f, -dy * 0.3f);
    }
    input->lastX = xpos;
    input->lastY = ypos;
}

void mouseButtonCallback(GLFWwindow* window, int button, int action, int /*mods*/) {
    auto* input = static_cast<InputState*>(glfwGetWindowUserPointer(window));
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        input->dragging = (action == GLFW_PRESS);
    }
}

void scrollCallback(GLFWwindow* window, double /*xoffset*/, double yoffset) {
    auto* input = static_cast<InputState*>(glfwGetWindowUserPointer(window));
    input->camera->zoom(static_cast<float>(-yoffset) * 0.2f);
}

void keyCallback(GLFWwindow* window, int key, int /*scancode*/, int action, int /*mods*/) {
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
}

struct CliArgs {
    std::string configPath = "configs/default.json";
    int overrideParticles = -1;
};

CliArgs parseArgs(int argc, char** argv) {
    CliArgs args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--particles" && i + 1 < argc) {
            args.overrideParticles = std::stoi(argv[++i]);
        } else if (arg == "--config" && i + 1 < argc) {
            args.configPath = argv[++i];
        }
    }
    return args;
}

} // namespace

int main(int argc, char** argv) {
    const CliArgs args = parseArgs(argc, argv);

    Config cfg = ConfigLoader::load(args.configPath);
    const int targetCount = args.overrideParticles > 0 ? args.overrideParticles : cfg.particleCount;
    std::vector<Particle> particles = initializeDamBreak(cfg, targetCount);

    if (!glfwInit()) {
        const char* desc = nullptr;
        glfwGetError(&desc);
        // GLFW does not silently fall back to a non-rendering backend,
        // by design, so a headless machine fails here rather than at
        // glfwCreateWindow() with a more confusing error.
        std::cerr << "glfwInit() failed: " << (desc ? desc : "unknown error") << "\n"
                  << "This requires a real display (X11/Wayland/macOS/Windows) with "
                     "working OpenGL 3.3+ drivers. See docs/architecture.md, "
                     "'Platform requirements for the viewer', for the packages a Linux/X11 machine "
                     "needs if this is that kind of failure.\n";
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(1024, 768, "AquaSPH -- fluid viewer", nullptr, nullptr);
    if (!window) {
        const char* desc = nullptr;
        glfwGetError(&desc);
        std::cerr << "glfwCreateWindow() failed: " << (desc ? desc : "unknown error") << "\n"
                  << "This requires a real display (X11/Wayland/macOS/Windows) with "
                     "working OpenGL 3.3+ drivers. See docs/architecture.md, "
                     "'Platform requirements for the viewer', for the packages a Linux/X11 machine "
                     "needs (libglfw3-dev or FetchContent + mesa-common-dev, "
                     "libgl1-mesa-dev, libxrandr-dev, libxinerama-dev, "
                     "libxcursor-dev, libxi-dev, libx11-dev) if this is that kind "
                     "of failure.\n";
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    if (!gl::loadGLFunctions()) {
        std::cerr << "Failed to resolve required OpenGL 3.3 core functions "
                     "(see the GLLoader lines above for which ones).\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    std::cout << "GL_RENDERER: " << gl::glGetString(gl::GL_RENDERER) << "\n";
    std::cout << "GL_VERSION:  " << gl::glGetString(gl::GL_VERSION) << "\n";

    // Domain center as the orbit target -- correct for any symmetric
    // domain_min/domain_max (the shipped configs/default.json is
    // [-1,-1,-1] to [1,1,1]); an asymmetric domain would want a
    // different target, but this project has never used one.
    const glm::vec3 target = (cfg.domainMin + cfg.domainMax) * 0.5f;
    const float domainSpan = glm::length(cfg.domainMax - cfg.domainMin);
    OrbitCamera camera(target, domainSpan);

    InputState input;
    input.camera = &camera;
    glfwSetWindowUserPointer(window, &input);
    glfwSetCursorPosCallback(window, cursorPosCallback);
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    glfwSetScrollCallback(window, scrollCallback);
    glfwSetKeyCallback(window, keyCallback);

    ParticleRenderer renderer(particles.size());

    CubicSplineKernel kernel(cfg.h);
    TaitEOS eos(cfg.restDensity, cfg.soundSpeed, cfg.gamma);
    ForceParams forceParams{cfg.viscosity, cfg.gravity};
    BoundaryBox bounds{cfg.domainMin, cfg.domainMax, cfg.wallDamping};
    PredictorCorrectorIntegrator integrator(bounds, cfg.maxSpeed);
    LinkedCell grid(cfg.domainMin, cfg.domainMax, cfg.h);

    const auto recompute = [&](std::vector<Particle>& p) {
        computeForces(p, grid, kernel, forceParams);
    };

    gl::glEnable(gl::GL_DEPTH_TEST);
    gl::glEnable(gl::GL_VERTEX_PROGRAM_POINT_SIZE);
    gl::glEnable(gl::GL_BLEND);
    gl::glBlendFunc(gl::GL_SRC_ALPHA, gl::GL_ONE_MINUS_SRC_ALPHA);
    gl::glClearColor(0.05f, 0.06f, 0.09f, 1.0f);

    double lastReportTime = glfwGetTime();
    int stepsSinceReport = 0;

    std::cout << "Controls: left-drag orbit, scroll zoom, ESC quit.\n";

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Exactly the four-call physics step main.cpp's benchmark loop
        // uses -- see main.cpp for why each call is there and in that
        // order (grid must be rebuilt before density, which must be
        // current before force, which the integrator needs before it
        // can re-evaluate forces at the predicted half-step).
        grid.build(particles);
        computeDensityPressure(particles, grid, kernel, eos);
        computeForces(particles, grid, kernel, forceParams);
        integrator.step(particles, cfg.dt, recompute);
        ++stepsSinceReport;

        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        if (width > 0 && height > 0) {
            gl::glViewport(0, 0, width, height);
        }
        gl::glClear(gl::GL_COLOR_BUFFER_BIT | gl::GL_DEPTH_BUFFER_BIT);

        renderer.updateParticles(particles, cfg.maxSpeed);
        const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
        renderer.draw(camera.viewMatrix(), camera.projectionMatrix(aspect), /*pointSizePixels=*/6.0f);

        glfwSwapBuffers(window);

        const double now = glfwGetTime();
        if (now - lastReportTime >= 1.0) {
            std::cout << "sim steps/sec: " << stepsSinceReport << "\n";
            stepsSinceReport = 0;
            lastReportTime = now;
        }
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
