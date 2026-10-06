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
// One-to-one presentation reads framebuffer words straight from buffer textures, the top and bottom
// halves in one each: the PS5 allows 1048576 texels per buffer texture, less than a 1080p frame.
const char* buffer_fragment_source = R"(#version 410 core
uniform samplerBuffer top;
uniform samplerBuffer bottom;
uniform ivec2 size;
uniform int split;
out vec4 color;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  int y = size.y - 1 - p.y;
  color = (y < split ? texelFetch(top, y * size.x + p.x) : texelFetch(bottom, (y - split) * size.x + p.x)).bgra;
}
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

GLuint link(GLuint vertex, GLuint fragment) {
  if (!vertex || !fragment) return 0;
  GLuint program = glCreateProgram();
  glAttachShader(program, vertex); glAttachShader(program, fragment);
  glLinkProgram(program);
  GLint linked = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked) {
    char log[2048] = {};
    glGetProgramInfoLog(program, sizeof(log), nullptr, log);
    std::fprintf(stderr, "Shader link failed: %s\n", log);
    glDeleteProgram(program);
    return 0;
  }
  return program;
}

} // namespace

bool GlPresenter::init(int width, int height) {
  if (width <= 0 || height <= 0 || program_) return false;
  width_ = width; height_ = height;
  synced_ = buffer_synced_ = false;
  GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source);
  GLuint fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
  GLuint buffer_fragment = compile(GL_FRAGMENT_SHADER, buffer_fragment_source);
  program_ = link(vertex, fragment);
  buffer_program_ = link(vertex, buffer_fragment);
  for (GLuint shader : {vertex, fragment, buffer_fragment}) if (shader) glDeleteShader(shader);
  if (!program_) {
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
  if (glGetError() != GL_NO_ERROR) return false;
  GLint max_texels = 0;
  glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &max_texels);
  split_ = (height + 1) / 2;
  if (buffer_program_ && max_texels / width >= split_) {
    glGenBuffers(2, buffers_);
    glGenTextures(2, buffer_textures_);
    for (int i = 0; i < 2; ++i) {
      glBindBuffer(GL_TEXTURE_BUFFER, buffers_[i]);
      glBufferData(GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(width) * split_ * 4, nullptr, GL_DYNAMIC_DRAW);
      glBindTexture(GL_TEXTURE_BUFFER, buffer_textures_[i]);
      glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA8, buffers_[i]);
    }
    glBindBuffer(GL_TEXTURE_BUFFER, 0);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
  }
  // Without a buffer texture every size presents through the 2D texture.
  if (glGetError() != GL_NO_ERROR) release_buffer();
  return true;
}

bool GlPresenter::draw(const std::uint32_t* argb, int sw, int sh) {
  if (!program_ || !argb || sw <= 0 || sh <= 0) return false;
  synced_ = buffer_synced_ = false;
  if (one_to_one(sw, sh)) {
    upload_rows(argb, 0, height_);
  } else {
    glBindTexture(GL_TEXTURE_2D, texture_);
    upload(argb, {0, 0, width_, height_});
  }
  return present(sw, sh);
}

bool GlPresenter::draw(const std::uint32_t* argb, std::span<const ERRect> damage, std::span<const DamageMove> moves,
                       int sw, int sh) {
  if (!program_ || !argb || sw <= 0 || sh <= 0) return false;
  if (one_to_one(sw, sh)) {
    synced_ = false;
    if (!buffer_synced_) {
      upload_rows(argb, 0, height_);
      buffer_synced_ = true;
    } else {
      upload_changed_rows(argb, damage, moves);
    }
    return present(sw, sh);
  }
  buffer_synced_ = false;
  glBindTexture(GL_TEXTURE_2D, texture_);
  // A new surface size also covers fullscreen changes and lost drawables.
  if (!synced_ || sw != surface_width_ || sh != surface_height_) {
    upload(argb, {0, 0, width_, height_});
    synced_ = true;
    surface_width_ = sw; surface_height_ = sh;
  } else {
    for (const DamageMove& m : moves)
      if (!move(m)) {
        upload(argb, {0, 0, width_, height_});
        return present(sw, sh);
      }
    glBindTexture(GL_TEXTURE_2D, texture_);
    for (const ERRect& rect : damage) upload(argb, rect);
  }
  return present(sw, sh);
}

bool GlPresenter::one_to_one(int sw, int sh) const { return buffer_textures_[0] && sw == width_ && sh == height_; }

// The buffer holds whole framebuffer rows, so a move needs no GPU copy: the rows it wrote are uploaded
// like damage. Close row ranges go up as one call.
void GlPresenter::upload_changed_rows(const std::uint32_t* argb, std::span<const ERRect> damage,
                                      std::span<const DamageMove> moves) {
  struct Rows { int y0, y1; };
  Rows rows[ER_DAMAGE_RECTS_MAX + 8];
  int count = 0;
  for (const ERRect& r : damage) rows[count++] = {r.y, r.y + r.h};
  for (const DamageMove& m : moves) rows[count++] = {m.src.y + m.dy, m.src.y + m.dy + m.src.h};
  std::sort(rows, rows + count, [](const Rows& a, const Rows& b) { return a.y0 < b.y0; });
  constexpr int gap = 16;
  for (int i = 0; i < count;) {
    int y0 = rows[i].y0, y1 = rows[i].y1;
    for (++i; i < count && rows[i].y0 <= y1 + gap; ++i) y1 = std::max(y1, rows[i].y1);
    upload_rows(argb, std::max(y0, 0), std::min(y1, height_));
  }
}

void GlPresenter::upload_rows(const std::uint32_t* argb, int y0, int y1) {
  const GLsizeiptr row = static_cast<GLsizeiptr>(width_) * 4;
  for (int i = 0; i < 2; ++i) {
    const int first = i * split_, a = std::max(y0, first), b = std::min(y1, first + split_);
    if (b <= a) continue;
    glBindBuffer(GL_TEXTURE_BUFFER, buffers_[i]);
    glBufferSubData(GL_TEXTURE_BUFFER, (a - first) * row, (b - a) * row, argb + static_cast<std::size_t>(a) * width_);
  }
  glBindBuffer(GL_TEXTURE_BUFFER, 0);
}

void GlPresenter::upload(const std::uint32_t* argb, const ERRect& rect) {
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, width_);
  glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x, rect.y, rect.w, rect.h, GL_RGBA, GL_UNSIGNED_BYTE,
                  argb + static_cast<std::size_t>(rect.y) * width_ + rect.x);
}

// Texture rows are framebuffer rows, so framebuffer coordinates address both blits directly.
bool GlPresenter::move(const DamageMove& m) {
  if (!scratch_) {
    glGenTextures(1, &scratch_);
    glBindTexture(GL_TEXTURE_2D, scratch_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &read_fbo_);
    glGenFramebuffers(1, &draw_fbo_);
  }
  const ERRect& r = m.src;
  const auto blit = [&](unsigned int from, unsigned int to, int dx, int dy) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo_);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, from, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo_);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, to, 0);
    glBlitFramebuffer(r.x, r.y, r.x + r.w, r.y + r.h, r.x + dx, r.y + dy, r.x + dx + r.w, r.y + dy + r.h,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
  };
  blit(texture_, scratch_, 0, 0);
  blit(scratch_, texture_, m.dx, m.dy);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return glGetError() == GL_NO_ERROR;
}

bool GlPresenter::present(int sw, int sh) {
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glBindVertexArray(vao_);
  glActiveTexture(GL_TEXTURE0);
  if (one_to_one(sw, sh)) {
    glViewport(0, 0, sw, sh);
    glBindTexture(GL_TEXTURE_BUFFER, buffer_textures_[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_BUFFER, buffer_textures_[1]);
    glUseProgram(buffer_program_);
    glUniform1i(glGetUniformLocation(buffer_program_, "top"), 0);
    glUniform1i(glGetUniformLocation(buffer_program_, "bottom"), 1);
    glUniform2i(glGetUniformLocation(buffer_program_, "size"), width_, height_);
    glUniform1i(glGetUniformLocation(buffer_program_, "split"), split_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    return glGetError() == GL_NO_ERROR;
  }
  glViewport(0, 0, sw, sh);
  glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
  const float scale = std::min(float(sw) / width_, float(sh) / height_);
  const int vw = int(width_ * scale), vh = int(height_ * scale);
  glViewport((sw-vw)/2, (sh-vh)/2, vw, vh);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glUseProgram(program_);
  glUniform1i(glGetUniformLocation(program_, "frame"), 0);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  return glGetError() == GL_NO_ERROR;
}

void GlPresenter::release_buffer() {
  if (buffer_textures_[0]) glDeleteTextures(2, buffer_textures_);
  if (buffers_[0]) glDeleteBuffers(2, buffers_);
  buffer_textures_[0] = buffer_textures_[1] = buffers_[0] = buffers_[1] = 0;
  buffer_synced_ = false;
}

void GlPresenter::release() {
  release_buffer();
  if (buffer_program_) glDeleteProgram(buffer_program_);
  buffer_program_ = 0;
  if (texture_) glDeleteTextures(1, &texture_);
  if (scratch_) glDeleteTextures(1, &scratch_);
  if (read_fbo_) glDeleteFramebuffers(1, &read_fbo_);
  if (draw_fbo_) glDeleteFramebuffers(1, &draw_fbo_);
  if (vao_) glDeleteVertexArrays(1, &vao_);
  if (program_) glDeleteProgram(program_);
  texture_ = vao_ = program_ = scratch_ = read_fbo_ = draw_fbo_ = 0;
  synced_ = false;
}
