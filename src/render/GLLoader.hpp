#pragma once
// Minimal, hand-rolled OpenGL 3.3 core function-pointer loader.

#include <cstddef>

namespace aquasph::gl {

// Khronos platform typedefs: fixed ABI types.
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

// Constants (verified against the Khronos OpenGL registry)
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

// Added for the screen-space fluid renderer: framebuffer objects,
// float textures and the state the SSFR passes need.
constexpr GLbitfield GL_STENCIL_BUFFER_BIT        = 0x00000400;
constexpr GLenum     GL_LINES                     = 0x0001;
constexpr GLenum     GL_TRIANGLES                 = 0x0004;
constexpr GLenum     GL_TRIANGLE_STRIP            = 0x0005;
constexpr GLenum     GL_ONE                       = 1;
constexpr GLenum     GL_ZERO                      = 0;
constexpr GLenum     GL_CULL_FACE                 = 0x0B44;
constexpr GLenum     GL_BACK                      = 0x0405;
constexpr GLenum     GL_LEQUAL                    = 0x0203;
constexpr GLenum     GL_ALWAYS                    = 0x0207;
constexpr GLenum     GL_MULTISAMPLE               = 0x809D;
constexpr GLenum     GL_UNSIGNED_INT              = 0x1405;
constexpr GLenum     GL_UNSIGNED_BYTE             = 0x1401;
constexpr GLenum     GL_ELEMENT_ARRAY_BUFFER      = 0x8893;
constexpr GLenum     GL_PACK_ALIGNMENT            = 0x0D05;
constexpr GLenum     GL_UNPACK_ALIGNMENT          = 0x0CF5;
constexpr GLenum     GL_TEXTURE_2D                = 0x0DE1;
constexpr GLenum     GL_TEXTURE0                  = 0x84C0;
constexpr GLenum     GL_TEXTURE_MAG_FILTER        = 0x2800;
constexpr GLenum     GL_TEXTURE_MIN_FILTER        = 0x2801;
constexpr GLenum     GL_TEXTURE_WRAP_S            = 0x2802;
constexpr GLenum     GL_TEXTURE_WRAP_T            = 0x2803;
constexpr GLenum     GL_NEAREST                   = 0x2600;
constexpr GLenum     GL_LINEAR                    = 0x2601;
constexpr GLenum     GL_CLAMP_TO_EDGE             = 0x812F;
constexpr GLenum     GL_RED                       = 0x1903;
constexpr GLenum     GL_RG                        = 0x8227;
constexpr GLenum     GL_RGB                       = 0x1907;
constexpr GLenum     GL_RGBA                      = 0x1908;
constexpr GLenum     GL_R32F                      = 0x822E;
constexpr GLenum     GL_RG32F                     = 0x8230;
constexpr GLenum     GL_RGBA16F                   = 0x881A;
constexpr GLenum     GL_RGBA8                     = 0x8058;
constexpr GLenum     GL_DEPTH_COMPONENT           = 0x1902;
constexpr GLenum     GL_DEPTH_COMPONENT24         = 0x81A6;
constexpr GLenum     GL_FRAMEBUFFER               = 0x8D40;
constexpr GLenum     GL_RENDERBUFFER              = 0x8D41;
constexpr GLenum     GL_COLOR_ATTACHMENT0         = 0x8CE0;
constexpr GLenum     GL_COLOR_ATTACHMENT1         = 0x8CE1;
constexpr GLenum     GL_DEPTH_ATTACHMENT          = 0x8D00;
constexpr GLenum     GL_FRAMEBUFFER_COMPLETE      = 0x8CD5;
constexpr GLenum     GL_MAX_TEXTURE_SIZE          = 0x0D33;

// Function pointer types
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

// Added for the screen-space fluid renderer
using PFN_glDrawElements           = void (*)(GLenum, GLsizei, GLenum, const void*);
using PFN_glDepthMask              = void (*)(GLboolean);
using PFN_glDepthFunc              = void (*)(GLenum);
using PFN_glCullFace               = void (*)(GLenum);
using PFN_glGetIntegerv            = void (*)(GLenum, GLint*);
using PFN_glPixelStorei            = void (*)(GLenum, GLint);
using PFN_glReadPixels             = void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
using PFN_glGenTextures            = void (*)(GLsizei, GLuint*);
using PFN_glBindTexture            = void (*)(GLenum, GLuint);
using PFN_glDeleteTextures         = void (*)(GLsizei, const GLuint*);
using PFN_glTexImage2D             = void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
using PFN_glTexParameteri          = void (*)(GLenum, GLenum, GLint);
using PFN_glActiveTexture          = void (*)(GLenum);
using PFN_glGenFramebuffers        = void (*)(GLsizei, GLuint*);
using PFN_glBindFramebuffer        = void (*)(GLenum, GLuint);
using PFN_glDeleteFramebuffers     = void (*)(GLsizei, const GLuint*);
using PFN_glFramebufferTexture2D   = void (*)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFN_glCheckFramebufferStatus = GLenum (*)(GLenum);
using PFN_glDrawBuffers            = void (*)(GLsizei, const GLenum*);
using PFN_glGenRenderbuffers       = void (*)(GLsizei, GLuint*);
using PFN_glBindRenderbuffer       = void (*)(GLenum, GLuint);
using PFN_glRenderbufferStorage    = void (*)(GLenum, GLenum, GLsizei, GLsizei);
using PFN_glFramebufferRenderbuffer = void (*)(GLenum, GLenum, GLenum, GLuint);
using PFN_glDeleteRenderbuffers    = void (*)(GLsizei, const GLuint*);
using PFN_glUniform1i              = void (*)(GLint, GLint);
using PFN_glUniform2f              = void (*)(GLint, GLfloat, GLfloat);
using PFN_glUniform4f              = void (*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);

// Global GL function pointers, named like the real GL functions so call sites read normally.
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

extern PFN_glDrawElements           glDrawElements;
extern PFN_glDepthMask              glDepthMask;
extern PFN_glDepthFunc              glDepthFunc;
extern PFN_glCullFace               glCullFace;
extern PFN_glGetIntegerv            glGetIntegerv;
extern PFN_glPixelStorei            glPixelStorei;
extern PFN_glReadPixels             glReadPixels;
extern PFN_glGenTextures            glGenTextures;
extern PFN_glBindTexture            glBindTexture;
extern PFN_glDeleteTextures         glDeleteTextures;
extern PFN_glTexImage2D             glTexImage2D;
extern PFN_glTexParameteri          glTexParameteri;
extern PFN_glActiveTexture          glActiveTexture;
extern PFN_glGenFramebuffers        glGenFramebuffers;
extern PFN_glBindFramebuffer        glBindFramebuffer;
extern PFN_glDeleteFramebuffers     glDeleteFramebuffers;
extern PFN_glFramebufferTexture2D   glFramebufferTexture2D;
extern PFN_glCheckFramebufferStatus glCheckFramebufferStatus;
extern PFN_glDrawBuffers            glDrawBuffers;
extern PFN_glGenRenderbuffers       glGenRenderbuffers;
extern PFN_glBindRenderbuffer       glBindRenderbuffer;
extern PFN_glRenderbufferStorage    glRenderbufferStorage;
extern PFN_glFramebufferRenderbuffer glFramebufferRenderbuffer;
extern PFN_glDeleteRenderbuffers    glDeleteRenderbuffers;
extern PFN_glUniform1i              glUniform1i;
extern PFN_glUniform2f              glUniform2f;
extern PFN_glUniform4f              glUniform4f;

// Resolves every pointer above via glfwGetProcAddress.
// Call exactly once, after a GL context is current and before any other GL call.
bool loadGLFunctions();

} // namespace aquasph::gl
