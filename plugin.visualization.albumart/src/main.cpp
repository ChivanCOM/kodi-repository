/*
 * ChivanCOM Album Art Visualizer for Kodi
 *
 * Layout (golden ratio):
 *   - Album art: left side, 60% of usable height, aspect-correct
 *   - Right panel: Title (large) / Artist (italic) / Album — golden-ratio spacing
 *   - Font loaded from Kodi VFS (arial.ttf) via stb_truetype
 */

#include <kodi/addon-instance/Visualization.h>
#include <kodi/Filesystem.h>
#include <kodi/gui/gl/GL.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <string>
#include <vector>
#include <cstring>
#include <cmath>
#include <algorithm>

// ── GLSL shaders ──────────────────────────────────────────────────────────────

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

// ── Text texture ──────────────────────────────────────────────────────────────

struct TextTex
{
  GLuint id = 0;
  float  w  = 0.f;
  float  h  = 0.f;

  void destroy()
  {
    if (id) { glDeleteTextures(1, &id); id = 0; }
    w = h = 0.f;
  }
};

// ── Visualizer ────────────────────────────────────────────────────────────────

class CVisualizationAlbumArt
    : public kodi::addon::CAddonBase
    , public kodi::addon::CInstanceVisualization
{
public:
  CVisualizationAlbumArt() = default;

  ~CVisualizationAlbumArt() override
  {
    DeinitGL();
  }

  ADDON_STATUS Create() override
  {
    return ADDON_STATUS_OK;
  }

  bool Start(int /*channels*/, int /*samplesPerSec*/, int /*bitsPerSample*/,
             const std::string& /*songName*/) override
  {
    if (!m_glReady)
      InitGL();
    m_currentArt.clear();
    m_pendingLoad = true;
    m_pendingText = true;
    return true;
  }

  void Stop() override {}
  void AudioData(const float*, size_t) override {}
  bool IsDirty() override { return true; }

  bool UpdateAlbumart(const std::string& albumart) override
  {
    if (albumart != m_currentArt)
    {
      m_currentArt  = albumart;
      m_pendingLoad = true;
    }
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
    if (!m_glReady)
      return;

    int vw = Width(), vh = Height();
    if (vw < 1 || vh < 1)
      return;

    if (m_pendingLoad)
    {
      m_pendingLoad = false;
      LoadArtTexture(m_currentArt);
    }

    if (m_pendingText || vw != m_viewW || vh != m_viewH)
    {
      m_pendingText = false;
      RebuildLayout(vw, vh);
    }

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_program);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Draw album art
    if (m_artTex && m_texW > 0)
      DrawQuad(m_artTex, m_artX0, m_artY0, m_artX1, m_artY1, 1.0f);

    // Draw text
    DrawTextTex(m_texTitle,  m_titleX,  m_titleY,  m_titleX  + m_texTitle.w  * m_ndcPerPx, m_titleY  + m_texTitle.h  * m_ndcPerPxH);
    DrawTextTex(m_texArtist, m_artistX, m_artistY, m_artistX + m_texArtist.w * m_ndcPerPx, m_artistY + m_texArtist.h * m_ndcPerPxH);
    DrawTextTex(m_texAlbum,  m_albumX,  m_albumY,  m_albumX  + m_texAlbum.w  * m_ndcPerPx, m_albumY  + m_texAlbum.h  * m_ndcPerPxH);

    glDisable(GL_BLEND);
    glUseProgram(0);
  }

private:
  static constexpr float kPhi = 1.6180339887f;

  // ── OpenGL helpers ─────────────────────────────────────────────────────────

  GLuint CompileShader(GLenum type, const char* src)
  {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
      char log[512] = {};
      glGetShaderInfoLog(s, sizeof(log), nullptr, log);
      kodi::Log(ADDON_LOG_ERROR, "[AlbumArt] shader: %s", log);
      glDeleteShader(s);
      return 0;
    }
    return s;
  }

  bool InitGL()
  {
    GLuint vs = CompileShader(GL_VERTEX_SHADER,   VERT_SRC);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, FRAG_SRC);
    if (!vs || !fs) return false;

    m_program = glCreateProgram();
    glAttachShader(m_program, vs);
    glAttachShader(m_program, fs);
    glBindAttribLocation(m_program, 0, "a_pos");
    glBindAttribLocation(m_program, 1, "a_uv");
    glLinkProgram(m_program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
    if (!ok)
    {
      kodi::Log(ADDON_LOG_ERROR, "[AlbumArt] link failed");
      return false;
    }
    m_locTex   = glGetUniformLocation(m_program, "u_tex");
    m_locAlpha = glGetUniformLocation(m_program, "u_alpha");

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

    m_glReady = true;
    return true;
  }

  void DeinitGL()
  {
    DeleteArtTexture();
    m_texTitle.destroy();
    m_texArtist.destroy();
    m_texAlbum.destroy();
    if (m_vbo)     { glDeleteBuffers(1, &m_vbo);      m_vbo = 0; }
#if !defined(HAS_GLES)
    if (m_vao)     { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
#endif
    if (m_program) { glDeleteProgram(m_program);      m_program = 0; }
    m_glReady = false;
  }

  void DrawQuad(GLuint tex, float x0, float y0, float x1, float y1, float alpha)
  {
    float verts[16] = {
      x0, y0,  0.f, 0.f,
      x1, y0,  1.f, 0.f,
      x1, y1,  1.f, 1.f,
      x0, y1,  0.f, 1.f,
    };
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(m_locTex,   0);
    glUniform1f(m_locAlpha, alpha);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
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

  void DrawTextTex(const TextTex& t, float x0, float y0, float x1, float y1)
  {
    if (!t.id || t.w <= 0.f) return;
    // UV: texture stored top-to-bottom; flip V so top of quad (y1) = v=0
    float verts[16] = {
      x0, y0,  0.f, 1.f,
      x1, y0,  1.f, 1.f,
      x1, y1,  1.f, 0.f,
      x0, y1,  0.f, 0.f,
    };
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glUniform1i(m_locTex,   0);
    glUniform1f(m_locAlpha, 1.0f);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
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

  // ── Art texture ────────────────────────────────────────────────────────────

  bool LoadArtTexture(const std::string& path)
  {
    DeleteArtTexture();
    if (path.empty()) return false;

    kodi::vfs::CFile file;
    if (!file.OpenFile(path, 0))
    {
      kodi::Log(ADDON_LOG_WARNING, "[AlbumArt] cannot open: %s", path.c_str());
      return false;
    }

    std::vector<uint8_t> buf;
    buf.reserve(512 * 1024);
    uint8_t chunk[8192];
    ssize_t n;
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

    m_texW  = w;
    m_texH  = h;
    m_viewW = 0;  // force layout recalc
    return true;
  }

  void DeleteArtTexture()
  {
    if (m_artTex) { glDeleteTextures(1, &m_artTex); m_artTex = 0; }
    m_texW = m_texH = 0;
  }

  // ── Font loading ───────────────────────────────────────────────────────────

  bool LoadFont()
  {
    if (!m_fontData.empty()) return true;

    static const char* candidates[] = {
      "special://xbmc/media/Fonts/arial.ttf",
      "special://xbmc/media/Fonts/NotoSans-Regular.ttf",
#if defined(TARGET_DARWIN)
      "/Library/Fonts/Arial.ttf",
      "/System/Library/Fonts/Supplemental/Arial.ttf",
#elif defined(TARGET_ANDROID)
      "/system/fonts/Roboto-Regular.ttf",
      "/system/fonts/DroidSans.ttf",
#else
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
#endif
      nullptr
    };

    for (int i = 0; candidates[i]; ++i)
    {
      kodi::vfs::CFile f;
      if (!f.OpenFile(candidates[i], 0))
        continue;
      std::vector<uint8_t> buf;
      buf.reserve(256 * 1024);
      uint8_t chunk[8192];
      ssize_t n;
      while ((n = f.Read(chunk, sizeof(chunk))) > 0)
        buf.insert(buf.end(), chunk, chunk + n);
      f.Close();
      if (buf.empty()) continue;

      stbtt_fontinfo info;
      if (!stbtt_InitFont(&info, buf.data(), 0)) continue;

      m_fontData = std::move(buf);
      m_fontInfo = info;
      kodi::Log(ADDON_LOG_INFO, "[AlbumArt] font: %s", candidates[i]);
      return true;
    }
    kodi::Log(ADDON_LOG_WARNING, "[AlbumArt] no font found");
    return false;
  }

  // ── Text rasterization ─────────────────────────────────────────────────────

  TextTex MakeTextTex(const std::string& text, float pixelH, bool italic)
  {
    TextTex out;
    if (text.empty() || m_fontData.empty()) return out;

    float scale = stbtt_ScaleForPixelHeight(&m_fontInfo, pixelH);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&m_fontInfo, &ascent, &descent, &lineGap);
    int asc  = (int)(ascent  * scale + 0.5f);
    int dsc  = (int)(descent * scale - 0.5f);  // negative
    int lineH = asc - dsc;

    // Measure total width
    int totalW = 0;
    for (unsigned char c : text)
    {
      if (c < 32 || c > 126) continue;
      int adv, lsb;
      stbtt_GetCodepointHMetrics(&m_fontInfo, c, &adv, &lsb);
      totalW += (int)(adv * scale + 0.5f);
    }
    if (totalW <= 0) return out;

    float shear  = italic ? 0.25f : 0.0f;
    int   extraR = italic ? (int)(asc      * shear + 1.f) : 0;
    int   extraL = italic ? (int)((-dsc)   * shear + 1.f) : 0;
    int   imgW   = totalW + extraL + extraR + 2;
    int   imgH   = lineH + 2;

    std::vector<uint8_t> bitmap(imgW * imgH, 0);

    int penX = extraL + 1;
    for (unsigned char c : text)
    {
      if (c < 32 || c > 126) continue;
      int adv, lsb;
      stbtt_GetCodepointHMetrics(&m_fontInfo, c, &adv, &lsb);

      int x0g, y0g, x1g, y1g;
      stbtt_GetCodepointBitmapBox(&m_fontInfo, c, scale, scale, &x0g, &y0g, &x1g, &y1g);
      int gw = x1g - x0g;
      int gh = y1g - y0g;

      if (gw > 0 && gh > 0)
      {
        std::vector<uint8_t> glyph(gw * gh);
        stbtt_MakeCodepointBitmap(&m_fontInfo, glyph.data(), gw, gh, gw, scale, scale, c);

        int dstX = penX + (int)(lsb * scale);
        int dstY = asc + y0g;

        for (int py = 0; py < gh; ++py)
        {
          int dstRow = dstY + py;
          if (dstRow < 0 || dstRow >= imgH) continue;
          int shiftX = italic ? (int)((asc - dstRow) * shear + 0.5f) : 0;

          for (int px = 0; px < gw; ++px)
          {
            int dstCol = dstX + px + shiftX;
            if (dstCol < 0 || dstCol >= imgW) continue;
            uint8_t val = glyph[py * gw + px];
            int idx = dstRow * imgW + dstCol;
            bitmap[idx] = (uint8_t)std::min(255, (int)bitmap[idx] + val);
          }
        }
      }

      penX += (int)(adv * scale + 0.5f);
    }

    // Grayscale → RGBA (white text, alpha = coverage)
    std::vector<uint8_t> rgba(imgW * imgH * 4);
    for (int i = 0; i < imgW * imgH; ++i)
    {
      rgba[i*4+0] = 255;
      rgba[i*4+1] = 255;
      rgba[i*4+2] = 255;
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

    out.w = (float)imgW;
    out.h = (float)imgH;
    return out;
  }

  // ── Layout rebuild ─────────────────────────────────────────────────────────

  void RebuildLayout(int vw, int vh)
  {
    m_viewW     = vw;
    m_viewH     = vh;
    m_ndcPerPx  = 2.f / (float)vw;
    m_ndcPerPxH = 2.f / (float)vh;

    // Margins: 5% horizontal, 7% vertical (NDC units)
    float mxNdc = 2.f * 0.05f;
    float myNdc = 2.f * 0.07f;

    // Art: 60% of usable height, aspect-correct square pixels
    float usableH = 2.f - 2.f * myNdc;
    float artHNdc = usableH * 0.60f;

    float artAR = (m_texW > 0 && m_texH > 0)
                    ? (float)m_texW / (float)m_texH
                    : 1.f;
    // Convert pixel AR to NDC AR (account for non-square viewport)
    float artWNdc = artHNdc * artAR * ((float)vh / (float)vw);
    artWNdc = std::min(artWNdc, 0.90f);  // cap at 90% NDC width

    // Art quad: left-aligned, vertically centred
    float artLeft = -1.f + mxNdc;
    m_artX0 = artLeft;
    m_artX1 = artLeft + artWNdc;
    m_artY0 = -artHNdc * 0.5f;
    m_artY1 =  artHNdc * 0.5f;

    // Text panel
    float gapNdc = 2.f * 0.05f;
    float textX0 = m_artX1 + gapNdc;
    float textX1 = 1.f - mxNdc * 0.8f;
    float textWNdc = textX1 - textX0;

    // Font sizes in pixels (golden ratio hierarchy, clamped)
    float szTitle  = std::max(18.f, std::min(68.f, (float)vh * 0.065f));
    float szArtist = szTitle  / kPhi;
    float szAlbum  = szArtist / kPhi;

    LoadFont();
    m_texTitle.destroy();
    m_texArtist.destroy();
    m_texAlbum.destroy();

    m_texTitle  = MakeTextTex(m_title.empty()  ? " " : m_title,  szTitle,  false);
    m_texArtist = MakeTextTex(m_artist.empty() ? " " : m_artist, szArtist, true);
    m_texAlbum  = MakeTextTex(m_album.empty()  ? " " : m_album,  szAlbum,  false);

    // Scale ndcPerPx down if any line overflows panel width
    m_ndcPerPx = 2.f / (float)vw;
    auto fitW = [&](const TextTex& t) {
      if (t.w <= 0.f) return;
      float needed = t.w * m_ndcPerPx;
      if (needed > textWNdc)
        m_ndcPerPx = textWNdc / t.w;
    };
    fitW(m_texTitle);
    fitW(m_texArtist);
    fitW(m_texAlbum);

    // Text heights in NDC
    float hTitle  = m_texTitle.h  * m_ndcPerPxH;
    float hArtist = m_texArtist.h * m_ndcPerPxH;
    float hAlbum  = m_texAlbum.h  * m_ndcPerPxH;

    // Golden-ratio gaps between lines
    float gap1   = hArtist * (kPhi - 1.f);
    float gap2   = hAlbum  * (kPhi - 1.f);
    float blockH = hTitle + gap1 + hArtist + gap2 + hAlbum;

    // Centre text block within art height
    float blockTop = m_artY1 - (artHNdc - blockH) * 0.5f;

    // Bottom-left NDC for each text quad
    m_titleY  = blockTop - hTitle;
    m_artistY = m_titleY - gap1 - hArtist;
    m_albumY  = m_artistY - gap2 - hAlbum;

    m_titleX = m_artistX = m_albumX = textX0;
  }

  // ── State ──────────────────────────────────────────────────────────────────

  bool   m_glReady     = false;
  bool   m_pendingLoad = false;
  bool   m_pendingText = false;

  GLuint m_program  = 0;
  GLuint m_vao      = 0;
  GLuint m_vbo      = 0;
  GLint  m_locTex   = -1;
  GLint  m_locAlpha = -1;

  // Art
  GLuint m_artTex = 0;
  int    m_texW = 0, m_texH = 0;
  float  m_artX0 = 0.f, m_artY0 = 0.f, m_artX1 = 0.f, m_artY1 = 0.f;

  // Text
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
  stbtt_fontinfo       m_fontInfo = {};
};

ADDONCREATOR(CVisualizationAlbumArt)
