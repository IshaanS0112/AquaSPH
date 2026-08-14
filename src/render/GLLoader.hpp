#pragma once
// Minimal, hand-rolled OpenGL 3.3 core function-pointer loader.
//
// WHY NOT SYSTEM <GL/gl.h>, OR GLAD/GLEW: modern OpenGL functions (almost
// anything past the OpenGL 1.1 fixed-function subset) aren't necessarily
// link-time symbols in the platform's GL library at all -- on every
// platform the *correct* way to obtain them is a runtime lookup
// (glXGetProcAddress / wglGetProcAddress / eglGetProcAddress, which GLFW
// wraps uniformly as glfwGetProcAddress). A loader is therefore the right
// tool on any machine, not a workaround. This file declares exactly the
// ~30 GL 3.3 core entry points ParticleRenderer/Shader actually call,
// rather than pulling in GLAD/GLEW as an external dependency for that
// small a surface. Every signature and enum value below was cross-checked
// against the Khronos OpenGL registry (registry.khronos.org/OpenGL-Refpages,
// KhronosGroup/OpenGL-Registry api/GL/glcorearb.h) rather than written
// from memory -- a wrong-but-valid enum constant produces no compiler
// diagnostic and can be silently incorrect indefinitely. See
// docs/architecture.md for one such error this check caught.
//
// Usage: call loadGLFunctions() exactly once, after
// glfwMakeContextCurrent(), and check its return value before issuing
// any other call in this namespace.

#include <cstddef>

namespace aquasph::gl {

// --- Khronos platform typedefs -----------------------------------------
// Fixed ABI types -- redeclaring these is standard practice for any
// hand-written GL loader (GLAD's generated headers do the same thing),
// not a deviation from the spec.
using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLint = int;
using GLsizei = int;
using GLuint = unsigned int;
using GLfloat = float;
using GLchar = char;
using GLubyte = unsigned char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;

// --- Constants (verified against the Khronos OpenGL registry) ---------
constexpr GLbitfield GL_DEPTH_BUFFER_BIT          = 0x00000100;
constexpr GLbitfield GL_COLOR_BUFFER_BIT          = 0x00004000;
constexpr GLboolean  GL_FALSE_V                   = 0;
constexpr GLboolean  GL_TRUE_V                    = 1;
constexpr GLenum     GL_POINTS                    = 0x0000;
constexpr GLenum     GL_SRC_ALPHA                 = 0x0302;
constexpr GLenum     GL_ONE_MINUS_SRC_ALPHA       = 0x0303;
constexpr GLenum     GL_DEPTH_TEST                = 0x0B71;
constexpr GLenum     GL_BLEND                     = 0x0BE2;
constexpr GLenum     GL_VENDOR                    = 0x1F00;
constexpr GLenum     GL_RENDERER                  = 0x1F01;
constexpr GLenum     GL_VERSION                   = 0x1F02;
constexpr GLenum     GL_FLOAT                     = 0x1406;
constexpr GLenum     GL_ARRAY_BUFFER              = 0x8892;
constexpr GLenum     GL_STATIC_DRAW               = 0x88E4;
constexpr GLenum     GL_DYNAMIC_DRAW              = 0x88E8;
constexpr GLenum     GL_FRAGMENT_SHADER           = 0x8B30;
constexpr GLenum     GL_VERTEX_SHADER             = 0x8B31;
constexpr GLenum     GL_COMPILE_STATUS            = 0x8B81;
constexpr GLenum     GL_LINK_STATUS               = 0x8B82;
constexpr GLenum     GL_INFO_LOG_LENGTH           = 0x8B84;
constexpr GLenum     GL_VERTEX_PROGRAM_POINT_SIZE = 0x8642;

// --- Function pointer types --------------------------------------------
using PFN_glGetString              = const GLubyte* (*)(GLenum name);
using PFN_glClearColor             = void (*)(GLfloat, GLfloat, GLfloat, GLfloat);
using PFN_glClear                  = void (*)(GLbitfield);
using PFN_glViewport               = void (*)(GLint, GLint, GLsizei, GLsizei);
using PFN_glEnable                 = void (*)(GLenum);
using PFN_glDisable                = void (*)(GLenum);
using PFN_glBlendFunc              = void (*)(GLenum, GLenum);
using PFN_glGenBuffers             = void (*)(GLsizei, GLuint*);
using PFN_glBindBuffer             = void (*)(GLenum, GLuint);
using PFN_glBufferData             = void (*)(GLenum, GLsizeiptr, const void*, GLenum);
using PFN_glBufferSubData          = void (*)(GLenum, GLintptr, GLsizeiptr, const void*);
using PFN_glDeleteBuffers          = void (*)(GLsizei, const GLuint*);
using PFN_glGenVertexArrays        = void (*)(GLsizei, GLuint*);
using PFN_glBindVertexArray        = void (*)(GLuint);
using PFN_glDeleteVertexArrays     = void (*)(GLsizei, const GLuint*);
using PFN_glVertexAttribPointer    = void (*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using PFN_glEnableVertexAttribArray  = void (*)(GLuint);
using PFN_glDisableVertexAttribArray = void (*)(GLuint);
using PFN_glDrawArrays             = void (*)(GLenum, GLint, GLsizei);
using PFN_glCreateShader           = GLuint (*)(GLenum);
using PFN_glShaderSource           = void (*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFN_glCompileShader          = void (*)(GLuint);
using PFN_glGetShaderiv            = void (*)(GLuint, GLenum, GLint*);
using PFN_glGetShaderInfoLog       = void (*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glDeleteShader           = void (*)(GLuint);
using PFN_glCreateProgram          = GLuint (*)();
using PFN_glAttachShader           = void (*)(GLuint, GLuint);
using PFN_glLinkProgram            = void (*)(GLuint);
using PFN_glGetProgramiv           = void (*)(GLuint, GLenum, GLint*);
using PFN_glGetProgramInfoLog      = void (*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glUseProgram             = void (*)(GLuint);
using PFN_glDeleteProgram          = void (*)(GLuint);
using PFN_glGetUniformLocation     = GLint (*)(GLuint, const GLchar*);
using PFN_glUniformMatrix4fv       = void (*)(GLint, GLsizei, GLboolean, const GLfloat*);
using PFN_glUniform1f              = void (*)(GLint, GLfloat);
using PFN_glUniform3f              = void (*)(GLint, GLfloat, GLfloat, GLfloat);

// --- Global function pointers ------------------------------------------
// Named identically to the real GL functions so call sites in
// Shader.cpp/ParticleRenderer.cpp read exactly like normal OpenGL code
// (and would need zero changes if this were ever swapped for real GLAD
// output).
extern PFN_glGetString              glGetString;
extern PFN_glClearColor             glClearColor;
extern PFN_glClear                  glClear;
extern PFN_glViewport               glViewport;
extern PFN_glEnable                 glEnable;
extern PFN_glDisable                glDisable;
extern PFN_glBlendFunc              glBlendFunc;
extern PFN_glGenBuffers             glGenBuffers;
extern PFN_glBindBuffer             glBindBuffer;
extern PFN_glBufferData             glBufferData;
extern PFN_glBufferSubData          glBufferSubData;
extern PFN_glDeleteBuffers          glDeleteBuffers;
extern PFN_glGenVertexArrays        glGenVertexArrays;
extern PFN_glBindVertexArray        glBindVertexArray;
extern PFN_glDeleteVertexArrays     glDeleteVertexArrays;
extern PFN_glVertexAttribPointer    glVertexAttribPointer;
extern PFN_glEnableVertexAttribArray  glEnableVertexAttribArray;
extern PFN_glDisableVertexAttribArray glDisableVertexAttribArray;
extern PFN_glDrawArrays             glDrawArrays;
extern PFN_glCreateShader           glCreateShader;
extern PFN_glShaderSource           glShaderSource;
extern PFN_glCompileShader          glCompileShader;
extern PFN_glGetShaderiv            glGetShaderiv;
extern PFN_glGetShaderInfoLog       glGetShaderInfoLog;
extern PFN_glDeleteShader           glDeleteShader;
extern PFN_glCreateProgram          glCreateProgram;
extern PFN_glAttachShader           glAttachShader;
extern PFN_glLinkProgram            glLinkProgram;
extern PFN_glGetProgramiv           glGetProgramiv;
extern PFN_glGetProgramInfoLog      glGetProgramInfoLog;
extern PFN_glUseProgram             glUseProgram;
extern PFN_glDeleteProgram          glDeleteProgram;
extern PFN_glGetUniformLocation     glGetUniformLocation;
extern PFN_glUniformMatrix4fv       glUniformMatrix4fv;
extern PFN_glUniform1f              glUniform1f;
extern PFN_glUniform3f              glUniform3f;

// Resolves every pointer above via glfwGetProcAddress. Must be called
// after glfwMakeContextCurrent(). Returns false -- and logs which symbol
// failed to stderr -- if any come back null, which in practice means a
// context older than 3.3 core.
bool loadGLFunctions();

} // namespace aquasph::gl
