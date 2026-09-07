#pragma once
#include <vector>
#include "GLLoader.hpp"

namespace aquasph {

// A framebuffer object with one or two colour attachments and an optional
// depth attachment.
//
// The screen-space fluid pipeline is a chain of full-screen passes, so
// nearly everything it does is "render into a texture and read it back in
// the next pass". Wrapping that in one type keeps the pass code readable
// and, more importantly, puts the completeness check in one place: an
// incomplete FBO does not raise an error, it silently discards every
// draw, and the resulting all-black output is indistinguishable from a
// shader bug.
class OffscreenTarget {
public:
    enum class Format {
        R32F,      // linear view-space depth, smoothed depth
        RG32F,     // thickness + thickness-weighted speed
        RGBA16F,   // HDR scene colour
        RGBA8,     // final LDR colour, and anything read back for capture
    };

    OffscreenTarget(int width, int height, Format format, bool withDepth);
    ~OffscreenTarget();

    OffscreenTarget(const OffscreenTarget&) = delete;
    OffscreenTarget& operator=(const OffscreenTarget&) = delete;

    void bind() const;
    static void bindDefault(int width, int height);

    void resize(int width, int height);

    unsigned int colorTexture() const { return color_; }
    unsigned int depthTexture() const { return depth_; }
    int width() const { return width_; }
    int height() const { return height_; }

    // Reads the colour attachment back as tightly packed RGB8, bottom row
    // first (OpenGL's own order), flipped to top row first so it can be
    // handed straight to a PNG writer.
    std::vector<unsigned char> readRGB() const;

private:
    void allocate();
    void destroy();

    int width_;
    int height_;
    Format format_;
    bool withDepth_;
    gl::GLuint fbo_ = 0;
    gl::GLuint color_ = 0;
    gl::GLuint depth_ = 0;
};

// A single triangle covering the viewport, used by every full-screen
// pass. One triangle rather than two: a quad's diagonal makes the GPU
// shade the pixels along it twice (quad-granularity derivatives straddle
// the seam), and there is nothing a quad does here that a single
// oversized triangle does not.
class FullScreenTriangle {
public:
    FullScreenTriangle();
    ~FullScreenTriangle();
    FullScreenTriangle(const FullScreenTriangle&) = delete;
    FullScreenTriangle& operator=(const FullScreenTriangle&) = delete;
    void draw() const;

private:
    gl::GLuint vao_ = 0;
};

} // namespace aquasph
