// AquaSPH -- scenario viewer and offline frame recorder.
//
// A separate executable from `aquasph` (main.cpp) on purpose: the
// headless solver and the whole test suite have no reason to link
// GLFW/OpenGL, and this file has no reason to carry the metrics
// reporting. What is NOT separate is the physics -- both binaries drive
// the same scene::Simulation over the same scenario file, so what is on
// screen is provably the simulation that was benchmarked and validated,
// not a parallel implementation that could drift.
//
// Requires a real display and OpenGL 3.3+ drivers; see
// docs/rendering.md for per-platform prerequisites and for what
// --record-headless does and does not remove.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#ifdef _OPENMP
#include <omp.h>
#endif

#include "scene/Scenario.hpp"
#include "scene/ScenarioLoader.hpp"
#include "scene/Simulation.hpp"
#include "scene/Quality.hpp"
#include "render/GLLoader.hpp"
#include "render/Camera.hpp"
#include "render/ParticleRenderer.hpp"
#include "render/FluidRenderer.hpp"
#include "render/Environment.hpp"
#include "render/OffscreenTarget.hpp"
#include "render/FrameCapture.hpp"

using namespace aquasph;

namespace {

struct InputState {
    bool dragging = false;
    double lastX = 0.0;
    double lastY = 0.0;
    OrbitCamera* camera = nullptr;
    bool paused = false;
    bool surfaceMode = true;
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
    if (button == GLFW_MOUSE_BUTTON_LEFT) input->dragging = (action == GLFW_PRESS);
}

void scrollCallback(GLFWwindow* window, double /*xoffset*/, double yoffset) {
    auto* input = static_cast<InputState*>(glfwGetWindowUserPointer(window));
    input->camera->zoom(static_cast<float>(-yoffset) * 0.2f);
}

void keyCallback(GLFWwindow* window, int key, int /*scancode*/, int action, int /*mods*/) {
    if (action != GLFW_PRESS) return;
    auto* input = static_cast<InputState*>(glfwGetWindowUserPointer(window));
    if (key == GLFW_KEY_ESCAPE) glfwSetWindowShouldClose(window, GLFW_TRUE);
    if (key == GLFW_KEY_SPACE) input->paused = !input->paused;
    // Points mode stays available at a keypress, not as a legacy path:
    // it is how you see the actual particle distribution when the
    // surface looks wrong, and showing both is what demonstrates what
    // the screen-space reconstruction is buying.
    if (key == GLFW_KEY_M) input->surfaceMode = !input->surfaceMode;
}

struct CliArgs {
    std::string scenario = "dam_break";
    std::string recordDir;
    Quality quality = Quality::Medium;
    int threads = -1;
    int width = 1280;
    int height = 720;
    float simulatedTime = -1.0f;
    bool headlessRecord = false;
    bool pointsMode = false;
    bool listScenarios = false;
    bool help = false;
};

void printUsage() {
    std::cout <<
        "AquaSPH viewer -- interactive display and offline frame capture\n\n"
        "Usage: aquasph_view [options]\n\n"
        "  --scenario NAME      scenario to run (default: dam_break)\n"
        "  --list-scenarios     list available scenarios and exit\n"
        "  --quality Q          low | medium | high (default: medium)\n"
        "  --render-mode M      surface | points (default: from the scenario file)\n"
        "  --threads N          OpenMP thread count\n"
        "  --time SECONDS       override the scenario's simulated duration\n"
        "  --size W H           window / framebuffer size\n"
        "  --record DIR         write PNG frames into DIR at the scenario's output cadence\n"
        "  --record-headless    render into an FBO with no visible window, then exit\n"
        "  --help               this message\n\n"
        "Controls: left-drag orbits, scroll zooms, SPACE pauses, M toggles\n"
        "surface/points, ESC quits.\n";
}

CliArgs parseArgs(int argc, char** argv) {
    CliArgs a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "[aquasph_view] " << what << " requires a value.\n";
                std::exit(2);
            }
            return argv[++i];
        };
        // std::stoi/stof throw on anything unparseable; an uncaught
        // exception out of argument parsing means `terminate called after
        // throwing` in response to a typo.
        auto asInt = [&](const char* what) -> int {
            const std::string v = next(what);
            try { return std::stoi(v); }
            catch (const std::exception&) {
                std::cerr << "[aquasph_view] " << what << " expects an integer, got '"
                          << v << "'.\n";
                std::exit(2);
            }
        };
        auto asFloat = [&](const char* what) -> float {
            const std::string v = next(what);
            try { return std::stof(v); }
            catch (const std::exception&) {
                std::cerr << "[aquasph_view] " << what << " expects a number, got '"
                          << v << "'.\n";
                std::exit(2);
            }
        };

        if (arg == "--scenario")             a.scenario = next("--scenario");
        else if (arg == "--record")          a.recordDir = next("--record");
        else if (arg == "--threads")         a.threads = asInt("--threads");
        else if (arg == "--time")            a.simulatedTime = asFloat("--time");
        else if (arg == "--record-headless") a.headlessRecord = true;
        else if (arg == "--list-scenarios")  a.listScenarios = true;
        else if (arg == "--help" || arg == "-h") a.help = true;
        else if (arg == "--size") {
            a.width = std::stoi(next("--size"));
            a.height = std::stoi(next("--size"));
        } else if (arg == "--quality") {
            const std::string q = next("--quality");
            if (!parseQuality(q, a.quality)) {
                std::cerr << "[aquasph_view] Unknown quality '" << q << "'.\n";
                std::exit(2);
            }
        } else if (arg == "--render-mode") {
            const std::string m = next("--render-mode");
            if (m == "points")       a.pointsMode = true;
            else if (m == "surface") a.pointsMode = false;
            else {
                std::cerr << "[aquasph_view] Unknown render mode '" << m
                          << "'. Use surface or points.\n";
                std::exit(2);
            }
        } else {
            std::cerr << "[aquasph_view] Unknown option '" << arg << "'. Try --help.\n";
            std::exit(2);
        }
    }
    return a;
}

// The final blit: draw the finished offscreen image over the default
// framebuffer. Kept here rather than in a render/ class because it is
// four lines of shader and belongs to presentation, not to the renderer.
const char* kPresentVS = R"GLSL(
#version 330 core
out vec2 vUV;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

const char* kPresentFS = R"GLSL(
#version 330 core
in vec2 vUV;
uniform sampler2D uImage;
out vec4 FragColor;
void main() { FragColor = vec4(texture(uImage, vUV).rgb, 1.0); }
)GLSL";

const char* kDisplayHelp =
    "This needs a display (X11/Wayland/macOS/Windows) with OpenGL 3.3+ drivers.\n"
    "On a headless Linux machine, a virtual display works:\n"
    "    xvfb-run -s \"-screen 0 1280x720x24\" ./aquasph_view --record-headless ...\n"
    "Debian/Ubuntu build prerequisites: libglfw3-dev mesa-common-dev libgl1-mesa-dev\n"
    "libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libx11-dev.\n"
    "See docs/rendering.md.\n";

} // namespace

int main(int argc, char** argv) {
    const CliArgs args = parseArgs(argc, argv);
    if (args.help) { printUsage(); return 0; }

    const auto dirs = ScenarioLoader::defaultSearchDirs();
    if (args.listScenarios) {
        for (const std::string& n : ScenarioLoader::listAvailable(dirs)) std::cout << n << "\n";
        return 0;
    }

    const std::string path = ScenarioLoader::resolve(args.scenario, dirs);
    if (path.empty()) {
        std::cerr << "[aquasph_view] Scenario '" << args.scenario << "' not found.\n";
        return 2;
    }
    Scenario scenario;
    std::string error;
    if (!ScenarioLoader::loadFile(path, scenario, error)) {
        std::cerr << "[aquasph_view] " << error << "\n";
        return 2;
    }
    scenario.numerics.resolutionScale = qualityScale(args.quality);
    if (args.simulatedTime > 0.0f) scenario.duration.simulatedTime = args.simulatedTime;

    bool surfaceMode = (scenario.render.mode == RenderSettings::Mode::Surface);
    if (args.pointsMode) surfaceMode = false;

    if (!glfwInit()) {
        const char* desc = nullptr;
        glfwGetError(&desc);
        // GLFW does not silently fall back to a non-rendering backend, by
        // design, so a headless machine fails here rather than at
        // glfwCreateWindow() with a more confusing error.
        std::cerr << "glfwInit() failed: " << (desc ? desc : "unknown error") << "\n"
                  << kDisplayHelp;
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    if (args.headlessRecord) {
        // HONEST ABOUT WHAT THIS DOES. GLFW cannot create an OpenGL
        // context without a window, so --record-headless creates a hidden
        // one and renders into an FBO: no window ever appears, nothing is
        // ever swapped to a front buffer, and the frame size is
        // independent of any screen. What it does NOT remove is the need
        // for a display connection -- on a machine with no X server, run
        // it under xvfb-run. Calling this "no window" would be accurate;
        // calling it "no display required" would not be.
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }

    GLFWwindow* window = glfwCreateWindow(args.width, args.height,
                                           ("AquaSPH -- " + scenario.name).c_str(),
                                           nullptr, nullptr);
    if (!window) {
        const char* desc = nullptr;
        glfwGetError(&desc);
        std::cerr << "glfwCreateWindow() failed: " << (desc ? desc : "unknown error") << "\n"
                  << kDisplayHelp;
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(args.headlessRecord ? 0 : 1);

    if (!gl::loadGLFunctions()) {
        std::cerr << "Failed to resolve required OpenGL 3.3 core functions "
                     "(see the GLLoader lines above for which ones).\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    std::cout << "GL_RENDERER: " << gl::glGetString(gl::GL_RENDERER) << "\n";
    std::cout << "GL_VERSION:  " << gl::glGetString(gl::GL_VERSION) << "\n";

#ifdef _OPENMP
    if (args.threads > 0) omp_set_num_threads(args.threads);
#endif

    Simulation sim(scenario);
    std::cout << "Scenario: " << scenario.name << " (" << tierLabel(scenario.tier) << ")\n"
              << "quality=" << qualityName(args.quality)
              << " | fluid=" << sim.stats().fluidCount
              << " | boundary=" << sim.boundaryCount()
              << " | mode=" << (surfaceMode ? "surface" : "points") << "\n";

    const CameraSpec& cam = scenario.camera;
    OrbitCamera camera(cam.target, cam.distance, cam.yawDeg, cam.pitchDeg, cam.fovDeg);

    InputState input;
    input.camera = &camera;
    input.surfaceMode = surfaceMode;
    glfwSetWindowUserPointer(window, &input);
    glfwSetCursorPosCallback(window, cursorPosCallback);
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    glfwSetScrollCallback(window, scrollCallback);
    glfwSetKeyCallback(window, keyCallback);

    int fbWidth = args.width, fbHeight = args.height;
    glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
    if (fbWidth <= 0 || fbHeight <= 0) { fbWidth = args.width; fbHeight = args.height; }

    std::unique_ptr<Environment> environment;
    std::unique_ptr<FluidRenderer> fluid;
    std::unique_ptr<ParticleRenderer> points;
    std::unique_ptr<OffscreenTarget> sceneTarget;
    std::unique_ptr<OffscreenTarget> outputTarget;
    try {
        environment = std::make_unique<Environment>();
        fluid = std::make_unique<FluidRenderer>(fbWidth, fbHeight);
        points = std::make_unique<ParticleRenderer>(std::max<size_t>(sim.particles().size(), 4096));
        sceneTarget = std::make_unique<OffscreenTarget>(fbWidth, fbHeight,
                                                         OffscreenTarget::Format::RGBA8, true);
        outputTarget = std::make_unique<OffscreenTarget>(fbWidth, fbHeight,
                                                          OffscreenTarget::Format::RGBA8, true);
    } catch (const std::exception& e) {
        std::cerr << "[aquasph_view] Renderer initialisation failed: " << e.what() << "\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    std::unique_ptr<Shader> present;
    std::unique_ptr<FullScreenTriangle> presentTri;
    if (!args.headlessRecord) {
        present = std::make_unique<Shader>(kPresentVS, kPresentFS);
        presentTri = std::make_unique<FullScreenTriangle>();
    }

    std::unique_ptr<FrameCapture> capture;
    if (!args.recordDir.empty()) capture = std::make_unique<FrameCapture>(args.recordDir);

    // Particle draw radius, in units of the particle spacing.
    //
    // MEASURED, not guessed. At 0.62 the spheres overlap along the lattice
    // axes but NOT along its diagonals (neighbours there are sqrt(3) times
    // further apart), so the depth image came out peppered with holes that
    // fell through to the background and rendered as black speckle across
    // the whole surface. 0.9 covers the 3D diagonal and closes them. Much
    // larger than that and the fluid visibly inflates past its own volume.
    FluidRenderer::Params fp;
    fp.particleRadius = sim.spacing() * 0.9f;
    fp.smoothIterations = qualitySmoothIterations(args.quality);
    fp.blurRadiusPixels = 6.0f;
    // The bilateral filter's depth-difference weight is in inverse metres,
    // so it has to scale with the scenario: a falloff tuned for a 2 m tank
    // would treat every depth step in a 9 cm droplet scene as continuous
    // and smooth the crown flat.
    fp.depthFalloff = 1.0f / std::max(sim.spacing() * 3.0f, 1.0e-4f);
    fp.refractionStrength = 0.045f;
    if (!sim.scenario().materials.empty()) {
        fp.absorption = sim.scenario().materials.front().absorption;
    }
    // Absorption coefficients are per metre, so a small scene needs a
    // longer effective path or every sheet reads as clear water.
    const glm::vec3 domainSpan = scenario.domain.max - scenario.domain.min;
    fp.thicknessScale = 2.0f / std::max(glm::length(domainSpan) * 0.25f, 1.0e-3f);

    const float nearPlane = 0.01f * std::max(glm::length(domainSpan), 0.1f);
    const float farPlane = 40.0f * std::max(glm::length(domainSpan), 0.1f);
    camera.setClipPlanes(nearPlane, farPlane);

    environment->updateObstacles(sim.particles(), sim.wallCount(), sim.boundaryCount());

    float nextFrameTime = 0.0f;
    const float frameInterval = std::max(scenario.duration.outputInterval, 1.0e-4f);
    const auto wallStart = std::chrono::high_resolution_clock::now();
    double lastReport = glfwGetTime();
    int stepsSinceReport = 0;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        if (!input.paused && !sim.finished()) {
            if (capture) {
                // RECORDING RUNS ON SIMULATED TIME, NOT WALL TIME. Frames
                // are emitted at the scenario's output cadence, so a clip
                // plays back at a defined rate regardless of how long each
                // step took to compute -- which is the difference between
                // a reproducible recording and one whose speed depends on
                // the machine that made it.
                while (!sim.finished() && sim.time() < nextFrameTime) {
                    sim.step();
                    ++stepsSinceReport;
                }
                nextFrameTime += frameInterval;
            } else {
                sim.step();
                ++stepsSinceReport;
            }
        }

        int width = fbWidth, height = fbHeight;
        glfwGetFramebufferSize(window, &width, &height);
        if (width > 0 && height > 0 && (width != fbWidth || height != fbHeight)) {
            fbWidth = width;
            fbHeight = height;
            fluid->resize(fbWidth, fbHeight);
            sceneTarget->resize(fbWidth, fbHeight);
            outputTarget->resize(fbWidth, fbHeight);
        }

        if (cam.orbitRateDegPerSec != 0.0f) {
            // Constant, slow, and driven by SIMULATED time so a recorded
            // orbit is identical between a fast machine and a slow one.
            camera.setYaw(cam.yawDeg + cam.orbitRateDegPerSec * sim.time());
        }

        const float aspect = static_cast<float>(fbWidth) / static_cast<float>(std::max(1, fbHeight));
        const glm::mat4 view = camera.viewMatrix();
        const glm::mat4 proj = camera.projectionMatrix(aspect);
        const float pointScale =
            static_cast<float>(fbHeight) / (2.0f * std::tan(glm::radians(cam.fovDeg) * 0.5f));

        if (input.surfaceMode) {
            environment->updateContactShadow(sim.particles(), sim.boundaryCount(),
                                              sim.scenario(), fp.particleRadius);
            environment->render(*sceneTarget, sim.scenario(), view, proj, camera.position(),
                                 pointScale, sim.boundarySpacing() * 1.05f);
            fluid->updateParticles(sim.particles(), sim.boundaryCount());
            fluid->render(*sceneTarget, *outputTarget, view, proj, nearPlane, farPlane,
                           glm::radians(cam.fovDeg), sim.scenario().lighting, fp);
        } else {
            environment->render(*outputTarget, sim.scenario(), view, proj, camera.position(),
                                 pointScale, sim.boundarySpacing() * 1.05f);
            outputTarget->bind();
            gl::glEnable(gl::GL_DEPTH_TEST);
            gl::glEnable(gl::GL_VERTEX_PROGRAM_POINT_SIZE);
            points->updateParticles(sim.particles(), sim.boundaryCount(),
                                     sim.scenario().render.referenceSpeed);
            points->draw(view, proj, sim.scenario().render.pointSizePixels);
        }

        if (capture && !input.paused) {
            capture->write(outputTarget->readRGB(), fbWidth, fbHeight);
        }

        // Blit the finished frame to the default framebuffer by drawing it
        // as a textured full-screen pass. Skipped entirely in headless
        // recording -- there is nothing to present to.
        if (!args.headlessRecord) {
            OffscreenTarget::bindDefault(fbWidth, fbHeight);
            gl::glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            gl::glClear(gl::GL_COLOR_BUFFER_BIT | gl::GL_DEPTH_BUFFER_BIT);
            gl::glDisable(gl::GL_DEPTH_TEST);
            present->use();
            present->setTexture("uImage", 0, outputTarget->colorTexture());
            presentTri->draw();
            glfwSwapBuffers(window);
        }

        const double now = glfwGetTime();
        if (now - lastReport >= 1.0) {
            std::cout << "t=" << std::fixed << std::setprecision(3) << sim.time()
                      << " s | sim steps/s: " << stepsSinceReport
                      << " | fluid: " << sim.stats().fluidCount;
            if (capture) std::cout << " | frames: " << capture->framesWritten();
            std::cout << "\n" << std::defaultfloat << std::flush;
            stepsSinceReport = 0;
            lastReport = now;
        }

        if (sim.finished() && (capture || args.headlessRecord)) break;
    }

    const auto wallEnd = std::chrono::high_resolution_clock::now();
    const double wallSeconds = std::chrono::duration<double>(wallEnd - wallStart).count();

    // Every published result must be able to state what produced it.
    std::cout << "\n=== render summary ===\n";
    std::cout << "Scenario:        " << scenario.name << " (" << tierLabel(scenario.tier) << ")\n";
    std::cout << "Quality:         " << qualityName(args.quality) << "\n";
    std::cout << "Mode:            " << (input.surfaceMode ? "surface" : "points") << "\n";
    std::cout << "Resolution:      " << fbWidth << "x" << fbHeight << "\n";
    std::cout << "Fluid particles: " << sim.stats().fluidCount
              << " (boundary " << sim.boundaryCount() << ")\n";
    std::cout << "Simulated time:  " << sim.time() << " s over " << sim.stepCount() << " steps\n";
    std::cout << "Wall time:       " << std::fixed << std::setprecision(1) << wallSeconds
              << " s\n" << std::defaultfloat;
    std::cout << "Offline render:  " << ((capture || args.headlessRecord) ? "yes" : "no -- interactive")
              << "\n";
    if (capture) {
        std::cout << "Frames written:  " << capture->framesWritten()
                  << " to " << capture->directory() << "\n";
        std::cout << "Assemble with:   scripts/make_video.sh " << capture->directory() << "\n";
    }

    // TEARDOWN ORDER IS LOAD-BEARING. Every one of these owns GL objects
    // (textures, framebuffers, buffers, programs) and frees them in its
    // destructor. glfwTerminate() destroys the context and unloads the
    // driver, so a destructor that runs after it calls through a function
    // pointer into unmapped memory -- which is exactly what happened:
    // every run segfaulted on exit, after the summary had already printed,
    // so it looked like a clean run with a stray crash rather than a
    // lifetime bug. Releasing them explicitly, while the context is still
    // current, is the fix; leaving them to end-of-main destruction is not.
    capture.reset();
    present.reset();
    presentTri.reset();
    outputTarget.reset();
    sceneTarget.reset();
    points.reset();
    fluid.reset();
    environment.reset();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
