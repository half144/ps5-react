// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "gl_presenter.hpp"

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#elif defined(PROSPERO)
#include <GL/glcorearb.h>
#else
#define GL_GLEXT_PROTOTYPES
#include <SDL2/SDL_opengl.h>
#endif

#include <algorithm>
#include <cstdio>

namespace {
const char* vertex_source = R"(#version 410 core
out vec2 uv;
void main() {
  // Oversized triangle: one draw, no vertex buffer, no seam through the image.
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
  uv = vec2(p.x, 1.0 - p.y);
}
)";
const char* fragment_source = R"(#version 410 core
in vec2 uv;
uniform sampler2D frame;
out vec4 color;
// The texture holds 0xAARRGGBB words uploaded as RGBA bytes; little-endian
// storage puts them in B,G,R,A order.
void main() { color = texture(frame, uv).bgra; }
)";

GLuint compile(GLenum type, const char* source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    std::fprintf(stderr, "Shader compilation failed: %s\n", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}
} // namespace

bool GlPresenter::init(int width, int height) {
  if (width <= 0 || height <= 0 || program_) return false;
  width_ = width; height_ = height;
  synced_ = false;
  GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source);
  GLuint fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
  if (!vertex || !fragment) {
    if (vertex) glDeleteShader(vertex);
    if (fragment) glDeleteShader(fragment);
    return false;
  }
  program_ = glCreateProgram();
  glAttachShader(program_, vertex); glAttachShader(program_, fragment);
  glLinkProgram(program_);
  glDeleteShader(vertex); glDeleteShader(fragment);
  GLint linked = 0;
  glGetProgramiv(program_, GL_LINK_STATUS, &linked);
  if (!linked) {
    char log[2048] = {};
    glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
    std::fprintf(stderr, "Shader link failed: %s\n", log);
    release();
    return false;
  }
  glGenVertexArrays(1, &vao_);
  glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  return glGetError() == GL_NO_ERROR;
}

bool GlPresenter::draw(const std::uint32_t* argb, int sw, int sh) {
  if (!program_ || !argb || sw <= 0 || sh <= 0) return false;
  glBindTexture(GL_TEXTURE_2D, texture_);
  upload(argb, {0, 0, width_, height_});
  synced_ = false;
  return present(sw, sh);
}

bool GlPresenter::draw(const std::uint32_t* argb, std::span<const ERRect> damage, int sw, int sh) {
  if (!program_ || !argb || sw <= 0 || sh <= 0) return false;
  glBindTexture(GL_TEXTURE_2D, texture_);
  // A new surface size also covers fullscreen changes and lost drawables.
  if (!synced_ || sw != surface_width_ || sh != surface_height_) {
    upload(argb, {0, 0, width_, height_});
    synced_ = true;
    surface_width_ = sw; surface_height_ = sh;
  } else {
    for (const ERRect& rect : damage) upload(argb, rect);
  }
  return present(sw, sh);
}

void GlPresenter::upload(const std::uint32_t* argb, const ERRect& rect) {
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, width_);
  glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x, rect.y, rect.w, rect.h, GL_RGBA, GL_UNSIGNED_BYTE,
                  argb + static_cast<std::size_t>(rect.y) * width_ + rect.x);
}

bool GlPresenter::present(int sw, int sh) {
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glViewport(0, 0, sw, sh);
  glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
  const float scale = std::min(float(sw) / width_, float(sh) / height_);
  const int vw = int(width_ * scale), vh = int(height_ * scale);
  glViewport((sw-vw)/2, (sh-vh)/2, vw, vh);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glUseProgram(program_);
  glUniform1i(glGetUniformLocation(program_, "frame"), 0);
  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  return glGetError() == GL_NO_ERROR;
}

void GlPresenter::release() {
  if (texture_) glDeleteTextures(1, &texture_);
  if (vao_) glDeleteVertexArrays(1, &vao_);
  if (program_) glDeleteProgram(program_);
  texture_ = vao_ = program_ = 0;
  synced_ = false;
}
