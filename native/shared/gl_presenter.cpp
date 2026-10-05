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
#include <iterator>

namespace {
const char* vertex_source = R"(#version 410 core
// Texture rect drawn over the viewport: origin and size, normalized.
uniform vec4 uv_rect;
out vec2 uv;
void main() {
  // Oversized triangle: one draw, no vertex buffer, no seam through the image.
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
  uv = uv_rect.xy + vec2(p.x, 1.0 - p.y) * uv_rect.zw;
}
)";

constexpr int kLayerStep = 1024; // layer texture rows grow in steps of this many

void set_filtering() {
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}
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
  uv_rect_ = glGetUniformLocation(program_, "uv_rect");
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_);
  glGenVertexArrays(1, &vao_);
  glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  set_filtering();
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  return glGetError() == GL_NO_ERROR;
}

bool GlPresenter::draw(const std::uint32_t* argb, int sw, int sh) {
  if (!program_ || !argb || sw <= 0 || sh <= 0) return false;
  glBindTexture(GL_TEXTURE_2D, texture_);
  upload(argb, width_, 0, 0, width_, height_);
  synced_ = false;
  return present(sw, sh, nullptr);
}

bool GlPresenter::draw(const std::uint32_t* argb, std::span<const ERRect> damage, std::span<const DamageMove> moves,
                       const ERScrollLayer* layer, int sw, int sh) {
  if (!program_ || !argb || sw <= 0 || sh <= 0) return false;
  glBindTexture(GL_TEXTURE_2D, texture_);
  // A new surface size also covers fullscreen changes and lost drawables.
  const bool all = !synced_ || sw != surface_width_ || sh != surface_height_;
  if (all) {
    upload(argb, width_, 0, 0, width_, height_);
    synced_ = true;
    surface_width_ = sw; surface_height_ = sh;
  } else {
    bool moved = true;
    for (const DamageMove& m : moves)
      if (moved && !move(m)) moved = false;
    glBindTexture(GL_TEXTURE_2D, texture_);
    if (!moved) upload(argb, width_, 0, 0, width_, height_);
    else upload_rows(argb, damage);
  }
  return present(sw, sh, layer);
}

// Copies h rows of w pixels (row stride `stride`, starting at `pixels`) to (x, y) of the bound texture,
// through a pixel buffer: on the PS5, glTexSubImage2D from client memory costs about 17 us per thousand
// pixels, while rows go into a buffer in well under one and the copy into the texture stays on the GPU.
// The buffer is orphaned first so the upload never waits for a frame still reading it. The rows start at
// the buffer's start: from an offset into it they came out black on macOS.
void GlPresenter::upload(const std::uint32_t* pixels, int stride, int x, int y, int w, int h) {
  if (!unpack_) glGenBuffers(1, &unpack_);
  const auto bytes = static_cast<GLsizeiptr>(h) * stride * 4;
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack_);
  glBufferData(GL_PIXEL_UNPACK_BUFFER, bytes, nullptr, GL_STREAM_DRAW);
  glBufferSubData(GL_PIXEL_UNPACK_BUFFER, 0, bytes, pixels);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
  glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

// Uploads the whole framebuffer rows the rects cover, each once.
void GlPresenter::upload_rows(const std::uint32_t* argb, std::span<const ERRect> rects) {
  const int rows = height_;
  int bands[2 * ER_DAMAGE_RECTS_MAX];
  int count = 0;
  for (const ERRect& r : rects) {
    const int y0 = std::max(r.y, 0), y1 = std::min(r.y + r.h, rows);
    if (y1 > y0 && count < static_cast<int>(std::size(bands))) { bands[count++] = y0; bands[count++] = y1; }
  }
  // Sort the [y0, y1) pairs by y0, then merge overlapping or touching ones.
  for (int i = 2; i < count; i += 2)
    for (int j = i; j > 0 && bands[j - 2] > bands[j]; j -= 2) {
      std::swap(bands[j - 2], bands[j]);
      std::swap(bands[j - 1], bands[j + 1]);
    }
  for (int i = 0; i < count;) {
    const int y0 = bands[i];
    int y1 = bands[i + 1];
    for (i += 2; i < count && bands[i] <= y1; i += 2) y1 = std::max(y1, bands[i + 1]);
    upload(argb + static_cast<std::size_t>(y0) * width_, width_, 0, y0, width_, y1 - y0);
  }
}

// A new width, or a taller page than the texture holds, reallocates it (rows grow in steps so that is
// rare) and the engine repaints what it shows.
bool GlPresenter::resize_layer(int w, int h, bool* kept) {
  if (h > max_texture_ || w > max_texture_) return false;
  *kept = w == layer_width_ && h <= layer_capacity_;
  if (*kept) return true;
  if (!layer_texture_) glGenTextures(1, &layer_texture_);
  glBindTexture(GL_TEXTURE_2D, layer_texture_);
  layer_width_ = w;
  layer_capacity_ = std::min((h + kLayerStep - 1) / kLayerStep * kLayerStep, max_texture_);
  set_filtering();
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, layer_width_, layer_capacity_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  return glGetError() == GL_NO_ERROR;
}

void GlPresenter::upload_layer(const std::uint32_t* pixels, int x, int y, int w, int h) {
  glBindTexture(GL_TEXTURE_2D, layer_texture_);
  upload(pixels, w, x, y, w, h);
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

bool GlPresenter::present(int sw, int sh, const ERScrollLayer* layer) {
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
  glUniform4f(uv_rect_, 0, 0, 1, 1);
  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  if (layer) {
    // The viewport's window of the layer, over the viewport. GL's y axis points up.
    const ERRect& v = layer->view;
    glViewport((sw - vw) / 2 + int(v.x * scale), (sh - vh) / 2 + int((height_ - v.y - v.h) * scale),
               int(v.w * scale), int(v.h * scale));
    glBindTexture(GL_TEXTURE_2D, layer_texture_);
    glUniform4f(uv_rect_, float(layer->src_x) / layer_width_, float(layer->src_y) / layer_capacity_,
                float(v.w) / layer_width_, float(v.h) / layer_capacity_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  return glGetError() == GL_NO_ERROR;
}

void GlPresenter::release() {
  if (texture_) glDeleteTextures(1, &texture_);
  if (layer_texture_) glDeleteTextures(1, &layer_texture_);
  if (unpack_) glDeleteBuffers(1, &unpack_);
  if (scratch_) glDeleteTextures(1, &scratch_);
  if (read_fbo_) glDeleteFramebuffers(1, &read_fbo_);
  if (draw_fbo_) glDeleteFramebuffers(1, &draw_fbo_);
  if (vao_) glDeleteVertexArrays(1, &vao_);
  if (program_) glDeleteProgram(program_);
  texture_ = vao_ = program_ = scratch_ = read_fbo_ = draw_fbo_ = layer_texture_ = unpack_ = 0;
  layer_width_ = layer_capacity_ = 0;
  synced_ = false;
}
