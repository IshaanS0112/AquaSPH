// GLFW_INCLUDE_NONE: tell glfw3.h not to pull in a client API header
// (GL/gl.h) on its own. This is GLFW's own documented way to pair with a
// custom loader (this file) instead of a system OpenGL header -- see
// https://www.glfw.org/docs/latest/build_guide.html#build_link_glad --
// glfwGetProcAddress is the whole point of using a loader instead of
// linking function names directly, since letting glfw3.h's own GL/gl.h
// include declare them first would create the exact redefinition
// this loader exists to avoid.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "GLLoader.hpp"
#include <cstdio>

namespace aquasph::gl {

PFN_glGetString              glGetString = nullptr;
PFN_glClearColor             glClearColor = nullptr;
PFN_glClear                  glClear = nullptr;
PFN_glViewport               glViewport = nullptr;
PFN_glEnable                 glEnable = nullptr;
PFN_glDisable                glDisable = nullptr;
PFN_glBlendFunc              glBlendFunc = nullptr;
PFN_glGenBuffers             glGenBuffers = nullptr;
PFN_glBindBuffer             glBindBuffer = nullptr;
PFN_glBufferData             glBufferData = nullptr;
PFN_glBufferSubData          glBufferSubData = nullptr;
PFN_glDeleteBuffers          glDeleteBuffers = nullptr;
PFN_glGenVertexArrays        glGenVertexArrays = nullptr;
PFN_glBindVertexArray        glBindVertexArray = nullptr;
PFN_glDeleteVertexArrays     glDeleteVertexArrays = nullptr;
PFN_glVertexAttribPointer    glVertexAttribPointer = nullptr;
PFN_glEnableVertexAttribArray  glEnableVertexAttribArray = nullptr;
PFN_glDisableVertexAttribArray glDisableVertexAttribArray = nullptr;
PFN_glDrawArrays             glDrawArrays = nullptr;
PFN_glCreateShader           glCreateShader = nullptr;
PFN_glShaderSource           glShaderSource = nullptr;
PFN_glCompileShader          glCompileShader = nullptr;
PFN_glGetShaderiv            glGetShaderiv = nullptr;
PFN_glGetShaderInfoLog       glGetShaderInfoLog = nullptr;
PFN_glDeleteShader           glDeleteShader = nullptr;
PFN_glCreateProgram          glCreateProgram = nullptr;
PFN_glAttachShader           glAttachShader = nullptr;
PFN_glLinkProgram            glLinkProgram = nullptr;
PFN_glGetProgramiv           glGetProgramiv = nullptr;
PFN_glGetProgramInfoLog      glGetProgramInfoLog = nullptr;
PFN_glUseProgram             glUseProgram = nullptr;
PFN_glDeleteProgram          glDeleteProgram = nullptr;
PFN_glGetUniformLocation     glGetUniformLocation = nullptr;
PFN_glUniformMatrix4fv       glUniformMatrix4fv = nullptr;
PFN_glUniform1f              glUniform1f = nullptr;
PFN_glUniform3f              glUniform3f = nullptr;

// Added for the screen-space fluid renderer.
PFN_glDrawElements              glDrawElements = nullptr;
PFN_glDepthMask                 glDepthMask = nullptr;
PFN_glDepthFunc                 glDepthFunc = nullptr;
PFN_glCullFace                  glCullFace = nullptr;
PFN_glGetIntegerv               glGetIntegerv = nullptr;
PFN_glPixelStorei               glPixelStorei = nullptr;
PFN_glReadPixels                glReadPixels = nullptr;
PFN_glGenTextures               glGenTextures = nullptr;
PFN_glBindTexture               glBindTexture = nullptr;
PFN_glDeleteTextures            glDeleteTextures = nullptr;
PFN_glTexImage2D                glTexImage2D = nullptr;
PFN_glTexParameteri             glTexParameteri = nullptr;
PFN_glActiveTexture             glActiveTexture = nullptr;
PFN_glGenFramebuffers           glGenFramebuffers = nullptr;
PFN_glBindFramebuffer           glBindFramebuffer = nullptr;
PFN_glDeleteFramebuffers        glDeleteFramebuffers = nullptr;
PFN_glFramebufferTexture2D      glFramebufferTexture2D = nullptr;
PFN_glCheckFramebufferStatus    glCheckFramebufferStatus = nullptr;
PFN_glDrawBuffers               glDrawBuffers = nullptr;
PFN_glGenRenderbuffers          glGenRenderbuffers = nullptr;
PFN_glBindRenderbuffer          glBindRenderbuffer = nullptr;
PFN_glRenderbufferStorage       glRenderbufferStorage = nullptr;
PFN_glFramebufferRenderbuffer   glFramebufferRenderbuffer = nullptr;
PFN_glDeleteRenderbuffers       glDeleteRenderbuffers = nullptr;
PFN_glUniform1i                 glUniform1i = nullptr;
PFN_glUniform2f                 glUniform2f = nullptr;
PFN_glUniform4f                 glUniform4f = nullptr;

namespace {
// Loads one symbol and reports its name on failure -- deliberately
// verbose (one line per missing symbol, not just a single "loading
// failed") because the alternative is a silent null-pointer crash the
// first time that particular GL call executes, which is a much worse
// debugging experience than a clear list at startup.
template <typename PFN>
bool loadOne(PFN& out, const char* name) {
    out = reinterpret_cast<PFN>(glfwGetProcAddress(name));
    if (!out) {
        std::fprintf(stderr, "GLLoader: failed to resolve %s\n", name);
        return false;
    }
    return true;
}
} // namespace

bool loadGLFunctions() {
    bool ok = true;
    ok &= loadOne(glGetString, "glGetString");
    ok &= loadOne(glClearColor, "glClearColor");
    ok &= loadOne(glClear, "glClear");
    ok &= loadOne(glViewport, "glViewport");
    ok &= loadOne(glEnable, "glEnable");
    ok &= loadOne(glDisable, "glDisable");
    ok &= loadOne(glBlendFunc, "glBlendFunc");
    ok &= loadOne(glGenBuffers, "glGenBuffers");
    ok &= loadOne(glBindBuffer, "glBindBuffer");
    ok &= loadOne(glBufferData, "glBufferData");
    ok &= loadOne(glBufferSubData, "glBufferSubData");
    ok &= loadOne(glDeleteBuffers, "glDeleteBuffers");
    ok &= loadOne(glGenVertexArrays, "glGenVertexArrays");
    ok &= loadOne(glBindVertexArray, "glBindVertexArray");
    ok &= loadOne(glDeleteVertexArrays, "glDeleteVertexArrays");
    ok &= loadOne(glVertexAttribPointer, "glVertexAttribPointer");
    ok &= loadOne(glEnableVertexAttribArray, "glEnableVertexAttribArray");
    ok &= loadOne(glDisableVertexAttribArray, "glDisableVertexAttribArray");
    ok &= loadOne(glDrawArrays, "glDrawArrays");
    ok &= loadOne(glCreateShader, "glCreateShader");
    ok &= loadOne(glShaderSource, "glShaderSource");
    ok &= loadOne(glCompileShader, "glCompileShader");
    ok &= loadOne(glGetShaderiv, "glGetShaderiv");
    ok &= loadOne(glGetShaderInfoLog, "glGetShaderInfoLog");
    ok &= loadOne(glDeleteShader, "glDeleteShader");
    ok &= loadOne(glCreateProgram, "glCreateProgram");
    ok &= loadOne(glAttachShader, "glAttachShader");
    ok &= loadOne(glLinkProgram, "glLinkProgram");
    ok &= loadOne(glGetProgramiv, "glGetProgramiv");
    ok &= loadOne(glGetProgramInfoLog, "glGetProgramInfoLog");
    ok &= loadOne(glUseProgram, "glUseProgram");
    ok &= loadOne(glDeleteProgram, "glDeleteProgram");
    ok &= loadOne(glGetUniformLocation, "glGetUniformLocation");
    ok &= loadOne(glUniformMatrix4fv, "glUniformMatrix4fv");
    ok &= loadOne(glUniform1f, "glUniform1f");
    ok &= loadOne(glUniform3f, "glUniform3f");

    // Screen-space fluid renderer: framebuffer objects, float textures,
    // and the extra uniform setters the SSFR passes need. All are GL 3.0/3.3
    // core, so a context that provides the block above provides these too --
    // but they are still resolved and checked individually rather than
    // assumed, for the same reason as the rest.
    ok &= loadOne(glDrawElements, "glDrawElements");
    ok &= loadOne(glDepthMask, "glDepthMask");
    ok &= loadOne(glDepthFunc, "glDepthFunc");
    ok &= loadOne(glCullFace, "glCullFace");
    ok &= loadOne(glGetIntegerv, "glGetIntegerv");
    ok &= loadOne(glPixelStorei, "glPixelStorei");
    ok &= loadOne(glReadPixels, "glReadPixels");
    ok &= loadOne(glGenTextures, "glGenTextures");
    ok &= loadOne(glBindTexture, "glBindTexture");
    ok &= loadOne(glDeleteTextures, "glDeleteTextures");
    ok &= loadOne(glTexImage2D, "glTexImage2D");
    ok &= loadOne(glTexParameteri, "glTexParameteri");
    ok &= loadOne(glActiveTexture, "glActiveTexture");
    ok &= loadOne(glGenFramebuffers, "glGenFramebuffers");
    ok &= loadOne(glBindFramebuffer, "glBindFramebuffer");
    ok &= loadOne(glDeleteFramebuffers, "glDeleteFramebuffers");
    ok &= loadOne(glFramebufferTexture2D, "glFramebufferTexture2D");
    ok &= loadOne(glCheckFramebufferStatus, "glCheckFramebufferStatus");
    ok &= loadOne(glDrawBuffers, "glDrawBuffers");
    ok &= loadOne(glGenRenderbuffers, "glGenRenderbuffers");
    ok &= loadOne(glBindRenderbuffer, "glBindRenderbuffer");
    ok &= loadOne(glRenderbufferStorage, "glRenderbufferStorage");
    ok &= loadOne(glFramebufferRenderbuffer, "glFramebufferRenderbuffer");
    ok &= loadOne(glDeleteRenderbuffers, "glDeleteRenderbuffers");
    ok &= loadOne(glUniform1i, "glUniform1i");
    ok &= loadOne(glUniform2f, "glUniform2f");
    ok &= loadOne(glUniform4f, "glUniform4f");
    return ok;
}

} // namespace aquasph::gl
