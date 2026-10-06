#include "Shader.hpp"
#include "GLLoader.hpp"
#include <stdexcept>
#include <vector>
#include <string>

namespace aquasph {

using namespace gl;

namespace {

unsigned int compileStage(GLenum stage, const std::string& src, const char* stageName) {
    const unsigned int shader = glCreateShader(stage);
    const char* srcPtr = src.c_str();
    const GLint srcLen = static_cast<GLint>(src.size());
    glShaderSource(shader, 1, &srcPtr, &srcLen);
    glCompileShader(shader);

    GLint success = GL_FALSE_V;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint logLen = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLen);
        std::vector<char> log(static_cast<size_t>(logLen > 0 ? logLen : 1));
        glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
        glDeleteShader(shader);
        throw std::runtime_error(std::string("Shader compile error (") + stageName + "): " + log.data());
    }
    return shader;
}

} // namespace

Shader::Shader(const std::string& vertexSrc, const std::string& fragmentSrc) {
    const unsigned int vs = compileStage(GL_VERTEX_SHADER, vertexSrc, "vertex");
    const unsigned int fs = compileStage(GL_FRAGMENT_SHADER, fragmentSrc, "fragment");

    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);

    GLint success = GL_FALSE_V;
    glGetProgramiv(program_, GL_LINK_STATUS, &success);

    // Shader objects are ref-counted by attachment; safe (and correct practice) to delete the
    // stage objects right after linking.
    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!success) {
        GLint logLen = 0;
        glGetProgramiv(program_, 0x8B84 /* GL_INFO_LOG_LENGTH */, &logLen);
        std::vector<char> log(static_cast<size_t>(logLen > 0 ? logLen : 1));
        glGetProgramInfoLog(program_, static_cast<GLsizei>(log.size()), nullptr, log.data());
        glDeleteProgram(program_);
        program_ = 0;
        throw std::runtime_error(std::string("Shader link error: ") + log.data());
    }
}

Shader::~Shader() {
    if (program_) {
        glDeleteProgram(program_);
    }
}

void Shader::use() const {
    glUseProgram(program_);
}

void Shader::setMat4(const char* name, const float* mat4ColumnMajor) const {
    const GLint loc = glGetUniformLocation(program_, name);
    glUniformMatrix4fv(loc, 1, GL_FALSE_V, mat4ColumnMajor);
}

void Shader::setFloat(const char* name, float value) const {
    const GLint loc = glGetUniformLocation(program_, name);
    glUniform1f(loc, value);
}

void Shader::setVec3(const char* name, float x, float y, float z) const {
    const GLint loc = glGetUniformLocation(program_, name);
    glUniform3f(loc, x, y, z);
}


void Shader::setInt(const char* name, int value) const {
    glUniform1i(glGetUniformLocation(program_, name), value);
}

void Shader::setVec2(const char* name, float x, float y) const {
    glUniform2f(glGetUniformLocation(program_, name), x, y);
}

void Shader::setVec4(const char* name, float x, float y, float z, float w) const {
    glUniform4f(glGetUniformLocation(program_, name), x, y, z, w);
}

void Shader::setTexture(const char* name, int unit, unsigned int texture) const {
    glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(glGetUniformLocation(program_, name), unit);
}

} // namespace aquasph
