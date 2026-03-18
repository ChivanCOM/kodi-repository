/*
 * ChivanCOM Album Art Visualizer for Kodi
 *
 * Layout (golden ratio):
 *   - Background: audio-reactive shader (adapted from "Simple audio visualizer" by chronos)
 *   - Album art: left side, 60% of usable height, aspect-correct
 *   - Right panel: Title (large) / Artist (italic) / Album — golden-ratio spacing
 *   - Font: bundled Roboto Regular + Italic via stb_truetype
 */

#include <kodi/addon-instance/Visualization.h>
#include <kodi/Filesystem.h>
#include <kodi/gui/gl/GL.h>
#include <kodi/AddonBase.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <string>
#include <vector>
#include <complex>
#include <chrono>
#include <cstring>
#include <cmath>
#include <algorithm>

// ── Shared vertex shader ─────────────────────────────────────────────────────

static const char* VERT_SRC =
#if defined(HAS_GLES)
  "precision mediump float;\n"
  "attribute vec2 a_pos;\n"
  "attribute vec2 a_uv;\n"
  "varying   vec2 v_uv;\n"
#else
  "#version 150\n"
  "in  vec2 a_pos;\n"
  "in  vec2 a_uv;\n"
  "out vec2 v_uv;\n"
#endif
  "void main() {\n"
  "  v_uv        = a_uv;\n"
  "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
  "}\n";

// ── Texture / text fragment shader ───────────────────────────────────────────

static const char* FRAG_SRC =
#if defined(HAS_GLES)
  "precision mediump float;\n"
  "varying   vec2      v_uv;\n"
  "uniform   sampler2D u_tex;\n"
  "uniform   float     u_alpha;\n"
  "void main() {\n"
  "  gl_FragColor = texture2D(u_tex, v_uv) * vec4(1.0, 1.0, 1.0, u_alpha);\n"
  "}\n";
#else
  "#version 150\n"
  "in      vec2      v_uv;\n"
  "out     vec4      fragColor;\n"
  "uniform sampler2D u_tex;\n"
  "uniform float     u_alpha;\n"
  "void main() {\n"
  "  fragColor = texture(u_tex, v_uv) * vec4(1.0, 1.0, 1.0, u_alpha);\n"
  "}\n";
#endif

// ── Background audio-reactive fragment shader ────────────────────────────────
// Adapted from "Simple audio visualizer" by chronos (Shadertoy)
// Audio texture layout: row 0 (y≈0.25) = FFT magnitude, row 1 (y≈0.75) = waveform

static const char* BG_FRAG_SRC =
#if defined(HAS_GLES)
  "precision mediump float;\n"
  "uniform vec2      iResolution;\n"
  "uniform float     iTime;\n"
  "uniform sampler2D iChannel3;\n"
  "\n"
  "vec3 B2_spline(vec3 x) {\n"
  "  vec3 t  = 3.0 * x;\n"
  "  vec3 b0 = step(0.0,t)     * step(0.0,1.0-t);\n"
  "  vec3 b1 = step(0.0,t-1.0) * step(0.0,2.0-t);\n"
  "  vec3 b2 = step(0.0,t-2.0) * step(0.0,3.0-t);\n"
  "  return 0.5*(b0*t*t + b1*(-2.0*t*t+6.0*t-3.0) + b2*(3.0-t)*(3.0-t));\n"
  "}\n"
  "void main() {\n"
  "  vec2 uv  = gl_FragCoord.xy / iResolution.xy;\n"
  "  vec2 cen = 2.0*uv - 1.0;\n"
  "  cen.x   *= iResolution.x / iResolution.y;\n"
  "  float dist2 = dot(cen,cen);\n"
  "  float cdist = smoothstep(0.0,1.0,dist2);\n"
  "  float arc   = abs(atan(cen.y,cen.x)/radians(360.0))+0.01;\n"
  "  float t    = iTime/100.0;\n"
  "  float poly = (1.0+sin(t*10.0))/2.0;\n"
  "  vec3 sa = fract(vec3(poly*uv.x-t)+vec3(0.0,-0.333,-0.667));\n"
  "  vec3 sp = B2_spline(sa);\n"
  "  float f = abs(cen.y);\n"
  "  vec3 base  = max(vec3(1.0)-f*sp, vec3(0.0));\n"
  "  vec3 flame = pow(base,vec3(3.0));\n"
  "  vec3 disc  = 0.20*base;\n"
  "  vec3 wave  = 0.10*base;\n"
  "  vec3 flash = 0.05*base;\n"
  "  float s1 = texture2D(iChannel3,vec2(abs(uv.x-0.5)+0.01,0.25)).x;\n"
  "  float s2 = texture2D(iChannel3,vec2(cdist,0.75)).x;\n"
  "  float s3 = texture2D(iChannel3,vec2(arc,  0.75)).x;\n"
  "  float dd = smoothstep(-0.2,-0.1,s3-dist2);\n"
  "  dd *= (1.0-dd);\n"
  "  vec3 color = vec3(0.0);\n"
  "  float v = abs(uv.y-0.5);\n"
  "  color += flame*smoothstep(v,v*8.0,s1);\n"
  "  color += disc *smoothstep(0.5,1.0,s2)*(1.0-cdist);\n"
  "  color += flash*smoothstep(0.5,1.0,s3)*cdist;\n"
  "  color += wave *dd;\n"
  "  color  = pow(max(color,vec3(0.0)),vec3(0.4545));\n"
  "  gl_FragColor = vec4(color,1.0);\n"
  "}\n";
#else
  "#version 150\n"
  "uniform vec2      iResolution;\n"
  "uniform float     iTime;\n"
  "uniform sampler2D iChannel3;\n"
  "out vec4 fragColor;\n"
  "\n"
  "vec3 B2_spline(vec3 x) {\n"
  "  vec3 t  = 3.0 * x;\n"
  "  vec3 b0 = step(0.0,t)     * step(0.0,1.0-t);\n"
  "  vec3 b1 = step(0.0,t-1.0) * step(0.0,2.0-t);\n"
  "  vec3 b2 = step(0.0,t-2.0) * step(0.0,3.0-t);\n"
  "  return 0.5*(b0*t*t + b1*(-2.0*t*t+6.0*t-3.0) + b2*(3.0-t)*(3.0-t));\n"
  "}\n"
  "void main() {\n"
  "  vec2 uv  = gl_FragCoord.xy / iResolution.xy;\n"
  "  vec2 cen = 2.0*uv - 1.0;\n"
  "  cen.x   *= iResolution.x / iResolution.y;\n"
  "  float dist2 = dot(cen,cen);\n"
  "  float cdist = smoothstep(0.0,1.0,dist2);\n"
  "  float arc   = abs(atan(cen.y,cen.x)/radians(360.0))+0.01;\n"
  "  float t    = iTime/100.0;\n"
  "  float poly = (1.0+sin(t*10.0))/2.0;\n"
  "  vec3 sa = fract(vec3(poly*uv.x-t)+vec3(0.0,-0.333,-0.667));\n"
  "  vec3 sp = B2_spline(sa);\n"
  "  float f = abs(cen.y);\n"
  "  vec3 base  = max(vec3(1.0)-f*sp, vec3(0.0));\n"
  "  vec3 flame = pow(base,vec3(3.0));\n"
  "  vec3 disc  = 0.20*base;\n"
  "  vec3 wave  = 0.10*base;\n"
  "  vec3 flash = 0.05*base;\n"
  "  float s1 = texture(iChannel3,vec2(abs(uv.x-0.5)+0.01,0.25)).x;\n"
  "  float s2 = texture(iChannel3,vec2(cdist,0.75)).x;\n"
  "  float s3 = texture(iChannel3,vec2(arc,  0.75)).x;\n"
  "  float dd = smoothstep(-0.2,-0.1,s3-dist2);\n"
  "  dd *= (1.0-dd);\n"
  "  vec3 color = vec3(0.0);\n"
  "  float v = abs(uv.y-0.5);\n"
  "  color += flame*smoothstep(v,v*8.0,s1);\n"
  "  color += disc *smoothstep(0.5,1.0,s2)*(1.0-cdist);\n"
  "  color += flash*smoothstep(0.5,1.0,s3)*cdist;\n"
  "  color += wave *dd;\n"
  "  color  = pow(max(color,vec3(0.0)),vec3(0.4545));\n"
  "  fragColor = vec4(color,1.0);\n"
  "}\n";
#endif

// ── FFT (Cooley-Tukey radix-2) ────────────────────────────────────────────────

static void ComputeFFTMagnitudes(const float* mono, int n, float* out)
{
  std::vector<std::complex<float>> a(n);
  for (int i = 0; i < n; i++)
  {
    float w = 0.5f * (1.f - cosf(2.f * (float)M_PI * i / (n - 1)));
    a[i] = {mono[i] * w, 0.f};
  }
  for (int i = 1, j = 0; i < n; i++)
  {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (int len = 2; len <= n; len <<= 1)
  {
    float ang = -2.f * (float)M_PI / len;
    std::complex<float> wlen(cosf(ang), sinf(ang));
    for (int i = 0; i < n; i += len)
    {
      std::complex<float> w(1.f, 0.f);
      for (int j = 0; j < len / 2; j++)
      {
        auto u = a[i + j];
        auto v = a[i + j + len / 2] * w;
        a[i + j]           = u + v;
        a[i + j + len / 2] = u - v;
        w *= wlen;
      }
    }
  }
  float scale = 2.f / n;
  for (int i = 0; i < n; i++)
    out[i] = std::abs(a[i]) * scale;
}

// ── Text texture ──────────────────────────────────────────────────────────────

struct TextTex
{
  GLuint id   = 0;
  float  w    = 0.f;
  float  h    = 0.f;
  float  xOff = 0.f;

  void destroy()
  {
    if (id) { glDeleteTextures(1, &id); id = 0; }
    w = h = xOff = 0.f;
  }
};

// ── Visualizer ────────────────────────────────────────────────────────────────

class CVisualizationAlbumArt
    : public kodi::addon::CAddonBase
    , public kodi::addon::CInstanceVisualization
{
public:
  CVisualizationAlbumArt() = default;

  ~CVisualizationAlbumArt() override { DeinitGL(); }

  ADDON_STATUS Create() override { return ADDON_STATUS_OK; }

  bool Start(int /*channels*/, int /*samplesPerSec*/, int /*bitsPerSample*/,
             const std::string& /*songName*/) override
  {
    if (!m_glReady) InitGL();
    m_currentArt.clear();
    m_pendingLoad = true;
    m_pendingText = true;
    m_startTime   = std::chrono::steady_clock::now();
    return true;
  }

  void Stop() override {}

  void AudioData(const float* data, size_t length) override
  {
    // Mix interleaved stereo → mono, clamp to kAudioW samples
    float mono[kAudioW];
    int count = std::min((int)length / 2, kAudioW);
    for (int i = 0; i < count; i++)
      mono[i] = (data[i * 2] + data[i * 2 + 1]) * 0.5f;
    for (int i = count; i < kAudioW; i++)
      mono[i] = 0.f;

    // Waveform: [-1,1] → [0,1]
    for (int i = 0; i < kAudioW; i++)
      m_waveData[i] = mono[i] * 0.5f + 0.5f;

    // Frequency: FFT magnitudes, normalised
    ComputeFFTMagnitudes(mono, kAudioW, m_freqData);
    float peak = 0.f;
    for (int i = 1; i < kAudioW / 2; i++) peak = std::max(peak, m_freqData[i]);
    if (peak > 0.f)
      for (int i = 0; i < kAudioW; i++) m_freqData[i] /= peak;

    m_audioTexDirty = true;
  }

  bool IsDirty() override { return true; }

  bool UpdateAlbumart(const std::string& albumart) override
  {
    if (albumart != m_currentArt) { m_currentArt = albumart; m_pendingLoad = true; }
    return true;
  }

  bool UpdateTrack(const kodi::addon::VisualizationTrack& track) override
  {
    m_title  = track.GetTitle();
    m_artist = track.GetArtist();
    m_album  = track.GetAlbum();
    m_pendingText = true;
    return true;
  }

  void Render() override
  {
    if (!m_glReady) return;
    int vw = Width(), vh = Height();
    if (vw < 1 || vh < 1) return;

    if (m_pendingLoad)  { m_pendingLoad = false; LoadArtTexture(m_currentArt); }
    if (m_pendingText || vw != m_viewW || vh != m_viewH)
      { m_pendingText = false; RebuildLayout(vw, vh); }
    if (m_audioTexDirty) { m_audioTexDirty = false; UploadAudioTex(); }

    // Background
    float elapsed = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - m_startTime).count();

    glUseProgram(m_bgProgram);
    glUniform2f(m_locResolution, (float)vw, (float)vh);
    glUniform1f(m_locTime, elapsed);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_audioTex);
    glUniform1i(m_locAudio, 0);
    DrawFullscreen();

    // Art + text
    glUseProgram(m_program);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (m_artTex && m_texW > 0)
      DrawQuad(m_artTex, m_artX0, m_artY0, m_artX1, m_artY1, 1.0f);

    DrawTextTex(m_texTitle,  m_titleX,  m_titleY,
                m_titleX  + m_texTitle.w  * m_ndcPerPx, m_titleY  + m_texTitle.h  * m_ndcPerPxH);
    DrawTextTex(m_texArtist, m_artistX, m_artistY,
                m_artistX + m_texArtist.w * m_ndcPerPx, m_artistY + m_texArtist.h * m_ndcPerPxH);
    DrawTextTex(m_texAlbum,  m_albumX,  m_albumY,
                m_albumX  + m_texAlbum.w  * m_ndcPerPx, m_albumY  + m_texAlbum.h  * m_ndcPerPxH);

    glDisable(GL_BLEND);
    glUseProgram(0);
  }

private:
  static constexpr float kPhi    = 1.6180339887f;
  static constexpr int   kAudioW = 512;

  // ── GL helpers ──────────────────────────────────────────────────────────────

  GLuint CompileShader(GLenum type, const char* src)
  {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
      char log[512] = {};
      glGetShaderInfoLog(s, sizeof(log), nullptr, log);
      kodi::Log(ADDON_LOG_ERROR, "[AlbumArt] shader: %s", log);
      glDeleteShader(s); return 0;
    }
    return s;
  }

  GLuint LinkProgram(const char* vs_src, const char* fs_src)
  {
    GLuint vs = CompileShader(GL_VERTEX_SHADER,   vs_src);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) { glDeleteShader(vs); glDeleteShader(fs); return 0; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_pos");
    glBindAttribLocation(prog, 1, "a_uv");
    glLinkProgram(prog);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { kodi::Log(ADDON_LOG_ERROR, "[AlbumArt] link failed"); glDeleteProgram(prog); return 0; }
    return prog;
  }

  bool InitGL()
  {
    m_program = LinkProgram(VERT_SRC, FRAG_SRC);
    if (!m_program) return false;
    m_locTex   = glGetUniformLocation(m_program, "u_tex");
    m_locAlpha = glGetUniformLocation(m_program, "u_alpha");

    m_bgProgram = LinkProgram(VERT_SRC, BG_FRAG_SRC);
    if (!m_bgProgram) return false;
    m_locResolution = glGetUniformLocation(m_bgProgram, "iResolution");
    m_locTime       = glGetUniformLocation(m_bgProgram, "iTime");
    m_locAudio      = glGetUniformLocation(m_bgProgram, "iChannel3");

    glGenBuffers(1, &m_vbo);
#if !defined(HAS_GLES)
    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
#endif
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    float tmp[16] = {};
    glBufferData(GL_ARRAY_BUFFER, sizeof(tmp), tmp, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
    glEnableVertexAttribArray(1);
#if !defined(HAS_GLES)
    glBindVertexArray(0);
#endif
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Audio texture: kAudioW × 2 RGBA
    // Row 0 (y≈0.25) = frequency,  Row 1 (y≈0.75) = waveform
    glGenTextures(1, &m_audioTex);
    glBindTexture(GL_TEXTURE_2D, m_audioTex);
    std::vector<uint8_t> blank(kAudioW * 2 * 4, 128);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kAudioW, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, blank.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    m_glReady = true;
    return true;
  }

  void UploadAudioTex()
  {
    std::vector<uint8_t> pixels(kAudioW * 2 * 4);
    for (int i = 0; i < kAudioW; i++)
    {
      uint8_t fv = (uint8_t)(std::min(1.f, m_freqData[i]) * 255.f);
      uint8_t wv = (uint8_t)(std::max(0.f, std::min(1.f, m_waveData[i])) * 255.f);
      int f4 = i * 4, w4 = (kAudioW + i) * 4;
      pixels[f4]=pixels[f4+1]=pixels[f4+2]=fv; pixels[f4+3]=255;
      pixels[w4]=pixels[w4+1]=pixels[w4+2]=wv; pixels[w4+3]=255;
    }
    glBindTexture(GL_TEXTURE_2D, m_audioTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kAudioW, 2, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  void DeinitGL()
  {
    DeleteArtTexture();
    m_texTitle.destroy(); m_texArtist.destroy(); m_texAlbum.destroy();
    if (m_audioTex)  { glDeleteTextures(1, &m_audioTex);   m_audioTex = 0; }
    if (m_vbo)       { glDeleteBuffers(1, &m_vbo);          m_vbo = 0; }
#if !defined(HAS_GLES)
    if (m_vao)       { glDeleteVertexArrays(1, &m_vao);     m_vao = 0; }
#endif
    if (m_bgProgram) { glDeleteProgram(m_bgProgram);        m_bgProgram = 0; }
    if (m_program)   { glDeleteProgram(m_program);          m_program = 0; }
    m_glReady = false;
  }

  void DrawFullscreen()
  {
    static const float kFull[16] = {
      -1.f,-1.f, 0.f,0.f,
       1.f,-1.f, 1.f,0.f,
       1.f, 1.f, 1.f,1.f,
      -1.f, 1.f, 0.f,1.f,
    };
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(kFull), kFull);
#if defined(HAS_GLES)
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
#else
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindVertexArray(0);
#endif
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  void DrawQuad(GLuint tex, float x0, float y0, float x1, float y1, float alpha)
  {
    float verts[16] = {
      x0,y0, 0.f,0.f,  x1,y0, 1.f,0.f,
      x1,y1, 1.f,1.f,  x0,y1, 0.f,1.f,
    };
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(m_locTex, 0); glUniform1f(m_locAlpha, alpha);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
#if defined(HAS_GLES)
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
#else
    glBindVertexArray(m_vao); glDrawArrays(GL_TRIANGLE_FAN, 0, 4); glBindVertexArray(0);
#endif
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindTexture(GL_TEXTURE_2D, 0);
  }

  void DrawTextTex(const TextTex& t, float x0, float y0, float x1, float y1)
  {
    if (!t.id || t.w <= 0.f) return;
    float verts[16] = {
      x0,y0, 0.f,1.f,  x1,y0, 1.f,1.f,
      x1,y1, 1.f,0.f,  x0,y1, 0.f,0.f,
    };
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glUniform1i(m_locTex, 0); glUniform1f(m_locAlpha, 1.0f);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
#if defined(HAS_GLES)
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
#else
    glBindVertexArray(m_vao); glDrawArrays(GL_TRIANGLE_FAN, 0, 4); glBindVertexArray(0);
#endif
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindTexture(GL_TEXTURE_2D, 0);
  }

  // ── Art texture ─────────────────────────────────────────────────────────────

  bool LoadArtTexture(const std::string& path)
  {
    DeleteArtTexture();
    if (path.empty()) return false;
    kodi::vfs::CFile file;
    if (!file.OpenFile(path, 0)) return false;
    std::vector<uint8_t> buf; buf.reserve(512 * 1024);
    uint8_t chunk[8192]; ssize_t n;
    while ((n = file.Read(chunk, sizeof(chunk))) > 0)
      buf.insert(buf.end(), chunk, chunk + n);
    file.Close();
    if (buf.empty()) return false;
    int w, h, comp;
    stbi_set_flip_vertically_on_load(true);
    unsigned char* data = stbi_load_from_memory(buf.data(), (int)buf.size(), &w, &h, &comp, 4);
    if (!data) return false;
    glGenTextures(1, &m_artTex);
    glBindTexture(GL_TEXTURE_2D, m_artTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(data);
    m_texW = w; m_texH = h; m_viewW = 0;
    return true;
  }

  void DeleteArtTexture()
  {
    if (m_artTex) { glDeleteTextures(1, &m_artTex); m_artTex = 0; }
    m_texW = m_texH = 0;
  }

  // ── Font loading ────────────────────────────────────────────────────────────

  bool LoadFontFromCandidates(const std::vector<std::string>& candidates,
                               std::vector<uint8_t>& outData, stbtt_fontinfo& outInfo)
  {
    for (const auto& path : candidates)
    {
      kodi::vfs::CFile f;
      if (!f.OpenFile(path, 0)) continue;
      std::vector<uint8_t> buf; buf.reserve(256 * 1024);
      uint8_t chunk[8192]; ssize_t n;
      while ((n = f.Read(chunk, sizeof(chunk))) > 0)
        buf.insert(buf.end(), chunk, chunk + n);
      f.Close();
      if (buf.empty()) continue;
      stbtt_fontinfo info;
      if (!stbtt_InitFont(&info, buf.data(), 0)) continue;
      outData = std::move(buf); outInfo = info;
      kodi::Log(ADDON_LOG_INFO, "[AlbumArt] font: %s", path.c_str());
      return true;
    }
    return false;
  }

  bool LoadFont()
  {
    if (!m_fontData.empty()) return true;
    std::vector<std::string> reg = {
      kodi::addon::GetAddonPath("fonts/Roboto-Regular.ttf"),
      "special://xbmc/media/Fonts/arial.ttf",
      "special://xbmc/media/Fonts/NotoSans-Regular.ttf",
    };
    std::vector<std::string> ital = {
      kodi::addon::GetAddonPath("fonts/Roboto-Italic.ttf"),
    };
#if defined(TARGET_DARWIN)
    reg.push_back("/Library/Fonts/Arial.ttf");
    reg.push_back("/System/Library/Fonts/Supplemental/Arial.ttf");
#elif defined(TARGET_ANDROID)
    reg.push_back("/system/fonts/Roboto-Regular.ttf");
    ital.push_back("/system/fonts/Roboto-Italic.ttf");
#else
    reg.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    reg.push_back("/usr/share/fonts/TTF/DejaVuSans.ttf");
#endif
    if (!LoadFontFromCandidates(reg, m_fontData, m_fontInfo))
    {
      kodi::Log(ADDON_LOG_WARNING, "[AlbumArt] no font found");
      return false;
    }
    LoadFontFromCandidates(ital, m_fontDataItalic, m_fontInfoItalic);
    return true;
  }

  // ── Text rasterization ──────────────────────────────────────────────────────

  TextTex MakeTextTex(const std::string& text, float pixelH, bool italic)
  {
    TextTex out;
    if (text.empty() || m_fontData.empty()) return out;
    bool useShear      = italic && m_fontDataItalic.empty();
    stbtt_fontinfo& fi = (italic && !m_fontDataItalic.empty()) ? m_fontInfoItalic : m_fontInfo;
    float scale = stbtt_ScaleForPixelHeight(&fi, pixelH);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&fi, &ascent, &descent, &lineGap);
    int asc   = (int)(ascent  * scale + 0.5f);
    int dsc   = (int)(descent * scale - 0.5f);
    int lineH = asc - dsc;
    int totalW = 0;
    for (unsigned char c : text)
    {
      if (c < 32 || c > 126) continue;
      int adv, lsb; stbtt_GetCodepointHMetrics(&fi, c, &adv, &lsb);
      totalW += (int)(adv * scale + 0.5f);
    }
    if (totalW <= 0) return out;
    float shear  = useShear ? 0.25f : 0.0f;
    int   extraR = useShear ? (int)(asc    * shear + 1.f) : 0;
    int   extraL = useShear ? (int)((-dsc) * shear + 1.f) : 0;
    int   imgW   = totalW + extraL + extraR + 2;
    int   imgH   = lineH + 2;
    std::vector<uint8_t> bitmap(imgW * imgH, 0);
    int penX = extraL + 1;
    for (unsigned char c : text)
    {
      if (c < 32 || c > 126) continue;
      int adv, lsb; stbtt_GetCodepointHMetrics(&fi, c, &adv, &lsb);
      int x0g, y0g, x1g, y1g;
      stbtt_GetCodepointBitmapBox(&fi, c, scale, scale, &x0g, &y0g, &x1g, &y1g);
      int gw = x1g - x0g, gh = y1g - y0g;
      if (gw > 0 && gh > 0)
      {
        std::vector<uint8_t> glyph(gw * gh);
        stbtt_MakeCodepointBitmap(&fi, glyph.data(), gw, gh, gw, scale, scale, c);
        int dstX = penX + (int)(lsb * scale), dstY = asc + y0g;
        for (int py = 0; py < gh; ++py)
        {
          int dstRow = dstY + py;
          if (dstRow < 0 || dstRow >= imgH) continue;
          int shiftX = useShear ? (int)((asc - dstRow) * shear + 0.5f) : 0;
          for (int px = 0; px < gw; ++px)
          {
            int dstCol = dstX + px + shiftX;
            if (dstCol < 0 || dstCol >= imgW) continue;
            int idx = dstRow * imgW + dstCol;
            bitmap[idx] = (uint8_t)std::min(255, (int)bitmap[idx] + (int)glyph[py * gw + px]);
          }
        }
      }
      penX += (int)(adv * scale + 0.5f);
    }
    std::vector<uint8_t> rgba(imgW * imgH * 4);
    for (int i = 0; i < imgW * imgH; ++i)
    {
      rgba[i*4+0] = rgba[i*4+1] = rgba[i*4+2] = 255;
      rgba[i*4+3] = bitmap[i];
    }
    glGenTextures(1, &out.id);
    glBindTexture(GL_TEXTURE_2D, out.id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, imgW, imgH, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    out.w = (float)imgW; out.h = (float)imgH; out.xOff = (float)extraL;
    return out;
  }

  // ── Layout rebuild ──────────────────────────────────────────────────────────

  void RebuildLayout(int vw, int vh)
  {
    m_viewW = vw; m_viewH = vh;
    m_ndcPerPx  = 2.f / (float)vw;
    m_ndcPerPxH = 2.f / (float)vh;

    float mxNdc   = 2.f * 0.05f;
    float myNdc   = 2.f * 0.07f;
    float usableH = 2.f - 2.f * myNdc;
    float artHNdc = usableH * 0.60f;
    float artAR   = (m_texW > 0 && m_texH > 0) ? (float)m_texW / (float)m_texH : 1.f;
    float artWNdc = std::min(artHNdc * artAR * ((float)vh / (float)vw), 0.90f);

    m_artX0 = -1.f + mxNdc;
    m_artX1 =  m_artX0 + artWNdc;
    m_artY0 = -artHNdc * 0.5f;
    m_artY1 =  artHNdc * 0.5f;

    float textX0   = m_artX1 + 2.f * 0.05f;
    float textX1   = 1.f - mxNdc * 0.8f;
    float textWNdc = textX1 - textX0;

    float szTitle  = std::max(18.f, std::min(68.f, (float)vh * 0.065f));
    float szArtist = szTitle  / kPhi;
    float szAlbum  = szArtist / kPhi;

    LoadFont();
    m_texTitle.destroy(); m_texArtist.destroy(); m_texAlbum.destroy();
    m_texTitle  = MakeTextTex(m_title.empty()  ? " " : m_title,  szTitle,  false);
    m_texArtist = MakeTextTex(m_artist.empty() ? " " : m_artist, szArtist, true);
    m_texAlbum  = MakeTextTex(m_album.empty()  ? " " : m_album,  szAlbum,  false);

    m_ndcPerPx = 2.f / (float)vw;
    auto fitW = [&](const TextTex& t) {
      float effW = t.w - t.xOff;
      if (effW <= 0.f) return;
      float needed = effW * m_ndcPerPx;
      if (needed > textWNdc) m_ndcPerPx = textWNdc / effW;
    };
    fitW(m_texTitle); fitW(m_texArtist); fitW(m_texAlbum);

    float hTitle  = m_texTitle.h  * m_ndcPerPxH;
    float hArtist = m_texArtist.h * m_ndcPerPxH;
    float hAlbum  = m_texAlbum.h  * m_ndcPerPxH;
    float gap     = (hArtist + hAlbum) * 0.5f * (kPhi - 1.f) * 0.5f;
    float blockH  = hTitle + gap + hArtist + gap + hAlbum;

    float blockTop = m_artY1 - (artHNdc - blockH) * 0.5f;
    m_titleY  = blockTop - hTitle;
    m_artistY = m_titleY  - gap - hArtist;
    m_albumY  = m_artistY - gap - hAlbum;

    m_titleX  = textX0 - m_texTitle.xOff  * m_ndcPerPx;
    m_artistX = textX0 - m_texArtist.xOff * m_ndcPerPx;
    m_albumX  = textX0 - m_texAlbum.xOff  * m_ndcPerPx;
  }

  // ── State ───────────────────────────────────────────────────────────────────

  bool   m_glReady       = false;
  bool   m_pendingLoad   = false;
  bool   m_pendingText   = false;
  bool   m_audioTexDirty = false;

  GLuint m_program  = 0;
  GLint  m_locTex   = -1;
  GLint  m_locAlpha = -1;

  GLuint m_bgProgram      = 0;
  GLint  m_locResolution  = -1;
  GLint  m_locTime        = -1;
  GLint  m_locAudio       = -1;

  GLuint m_vao = 0;
  GLuint m_vbo = 0;

  GLuint m_audioTex = 0;
  float  m_freqData[kAudioW] = {};
  float  m_waveData[kAudioW] = {};

  GLuint m_artTex = 0;
  int    m_texW = 0, m_texH = 0;
  float  m_artX0 = 0.f, m_artY0 = 0.f, m_artX1 = 0.f, m_artY1 = 0.f;

  TextTex m_texTitle, m_texArtist, m_texAlbum;
  float   m_titleX  = 0.f, m_titleY  = 0.f;
  float   m_artistX = 0.f, m_artistY = 0.f;
  float   m_albumX  = 0.f, m_albumY  = 0.f;
  float   m_ndcPerPx  = 0.f;
  float   m_ndcPerPxH = 0.f;

  int    m_viewW = 0, m_viewH = 0;

  std::string m_currentArt;
  std::string m_title, m_artist, m_album;

  std::vector<uint8_t> m_fontData;
  stbtt_fontinfo       m_fontInfo       = {};
  std::vector<uint8_t> m_fontDataItalic;
  stbtt_fontinfo       m_fontInfoItalic = {};

  std::chrono::steady_clock::time_point m_startTime;
};

ADDONCREATOR(CVisualizationAlbumArt)
