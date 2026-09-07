#pragma once
#include <string>

namespace aquasph {

// Compiles and links a GLSL vertex+fragment program from source strings
// (not files) -- this project's shaders are small enough that embedding
// them as raw string literals in ParticleRenderer.cpp is more robust
// than shipping/locating a .glsl file relative to wherever the binary
// happens to be run from, and keeps the renderer a single self-contained
// unit with no runtime asset path to get wrong.
class Shader {
public:
    // Throws std::runtime_error (with the GL compiler/linker info log)
    // on failure. Failing loudly at startup beats silently rendering a
    // black screen from a half-broken program.
    Shader(const std::string& vertexSrc, const std::string& fragmentSrc);
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    void use() const;

    // `mat4ColumnMajor` must point to 16 floats in column-major order --
    // exactly glm::mat4's own in-memory layout, so callers can pass
    // glm::value_ptr(m) directly with no conversion.
    void setMat4(const char* name, const float* mat4ColumnMajor) const;
    void setFloat(const char* name, float value) const;
    void setVec3(const char* name, float x, float y, float z) const;
    void setInt(const char* name, int value) const;
    void setVec2(const char* name, float x, float y) const;
    void setVec4(const char* name, float x, float y, float z, float w) const;

    // Binds `texture` to texture unit `unit` and points the sampler
    // uniform `name` at it. One call instead of the three-step
    // activate/bind/setInt dance repeated at every SSFR pass boundary,
    // where getting the order wrong silently samples the previous pass's
    // texture and produces a plausible-looking but wrong image.
    void setTexture(const char* name, int unit, unsigned int texture) const;

private:
    unsigned int program_ = 0;
};

} // namespace aquasph
