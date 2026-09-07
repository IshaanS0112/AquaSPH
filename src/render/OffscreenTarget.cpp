#include "OffscreenTarget.hpp"
#include <algorithm>
#include <stdexcept>
#include <string>

namespace aquasph {

using namespace gl;

namespace {

struct FormatInfo { GLenum internalFormat; GLenum format; GLenum type; };

FormatInfo formatInfo(OffscreenTarget::Format f) {
    switch (f) {
        case OffscreenTarget::Format::R32F:    return {GL_R32F, GL_RED, GL_FLOAT};
        case OffscreenTarget::Format::RG32F:   return {GL_RG32F, GL_RG, GL_FLOAT};
        case OffscreenTarget::Format::RGBA16F: return {GL_RGBA16F, GL_RGBA, GL_FLOAT};
        case OffscreenTarget::Format::RGBA8:   return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
    }
    return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
}

} // namespace

OffscreenTarget::OffscreenTarget(int width, int height, Format format, bool withDepth)
    : width_(std::max(1, width)), height_(std::max(1, height)),
      format_(format), withDepth_(withDepth) {
    allocate();
}

OffscreenTarget::~OffscreenTarget() { destroy(); }

void OffscreenTarget::destroy() {
    if (color_) { glDeleteTextures(1, &color_); color_ = 0; }
    if (depth_) { glDeleteTextures(1, &depth_); depth_ = 0; }
    if (fbo_)   { glDeleteFramebuffers(1, &fbo_); fbo_ = 0; }
}

void OffscreenTarget::allocate() {
    const FormatInfo fi = formatInfo(format_);

    glGenTextures(1, &color_);
    glBindTexture(GL_TEXTURE_2D, color_);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(fi.internalFormat), width_, height_, 0,
                  fi.format, fi.type, nullptr);
    // LINEAR everywhere: the smoothing and composite passes sample at
    // exact texel centres, so filtering is not doing interpolation work,
    // but linear keeps the refraction offset -- which deliberately samples
    // off-centre -- from stair-stepping.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // CLAMP_TO_EDGE, not repeat: a refraction offset near the frame edge
    // would otherwise wrap around and sample the opposite side of the
    // image, which reads as a bright seam along the border.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (withDepth_) {
        glGenTextures(1, &depth_);
        glBindTexture(GL_TEXTURE_2D, depth_);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_DEPTH_COMPONENT24), width_, height_, 0,
                      GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_, 0);
    if (withDepth_) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth_, 0);
    }
    const GLenum buffers[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, buffers);

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        // Loudly, at construction. An incomplete FBO discards every draw
        // silently, and the resulting black frame looks exactly like a
        // broken shader.
        throw std::runtime_error("Framebuffer incomplete (status 0x" +
                                  std::to_string(status) + ") at " +
                                  std::to_string(width_) + "x" + std::to_string(height_) +
                                  ". A float colour attachment is the usual cause on a "
                                  "driver without ARB_texture_float.");
    }
}

void OffscreenTarget::resize(int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    destroy();
    allocate();
}

void OffscreenTarget::bind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);
}

void OffscreenTarget::bindDefault(int width, int height) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
}

std::vector<unsigned char> OffscreenTarget::readRGB() const {
    std::vector<unsigned char> raw(static_cast<size_t>(width_) * height_ * 3);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    // Default pack alignment is 4; a width whose byte-length is not a
    // multiple of 4 would otherwise be read back with padding and every
    // row after the first would be skewed.
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width_, height_, GL_RGB, GL_UNSIGNED_BYTE, raw.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // OpenGL's origin is bottom-left; PNG's is top-left.
    std::vector<unsigned char> flipped(raw.size());
    const size_t stride = static_cast<size_t>(width_) * 3;
    for (int y = 0; y < height_; ++y) {
        const size_t src = static_cast<size_t>(height_ - 1 - y) * stride;
        std::copy(raw.begin() + static_cast<long>(src),
                   raw.begin() + static_cast<long>(src + stride),
                   flipped.begin() + static_cast<long>(static_cast<size_t>(y) * stride));
    }
    return flipped;
}

FullScreenTriangle::FullScreenTriangle() {
    // No vertex buffer at all: the vertex shader generates the three
    // clip-space corners from gl_VertexID. A VAO must still be bound for
    // a draw call to be legal in the core profile, so an empty one is.
    glGenVertexArrays(1, &vao_);
}

FullScreenTriangle::~FullScreenTriangle() {
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void FullScreenTriangle::draw() const {
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

} // namespace aquasph
