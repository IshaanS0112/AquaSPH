#pragma once
#include <string>

namespace aquasph {

// Compiles and links a GLSL vertex+fragment program from source strings (not files).
class Shader {
public:
    // Throws std::runtime_error (with the GL compiler/linker info log) on failure.
    Shader(const std::string& vertexSrc, const std::string& fragmentSrc);
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    void use() const;

    // `mat4ColumnMajor` must point to 16 floats in column-major order.
    void setMat4(const char* name, const float* mat4ColumnMajor) const;
    void setFloat(const char* name, float value) const;
    void setVec3(const char* name, float x, float y, float z) const;
    void setInt(const char* name, int value) const;
    void setVec2(const char* name, float x, float y) const;
    void setVec4(const char* name, float x, float y, float z, float w) const;

    // Binds `texture` to texture unit `unit` and points the sampler uniform `name` at it.
    void setTexture(const char* name, int unit, unsigned int texture) const;

private:
    unsigned int program_ = 0;
};

} // namespace aquasph
