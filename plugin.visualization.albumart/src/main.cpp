/*
 * ChivanCOM Album Art Visualizer for Kodi
 *
 * Layout (matches layout.svg):
 *   - Background: selectable audio-reactive shader (settings → Background Shader)
 *     rendered at half-res and blurred before compositing
 *   - Album art: left side, 42.5% of canvas height, aspect-correct, centred vertically
 *   - Right panel: Title (bold) / Artist (italic) / Album — SVG baseline-to-baseline spacing
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

// ── GLES 2.0 compatibility: define 16-bit float constants if missing ──────────
// GL_RGBA16F / GL_HALF_FLOAT are GLES 3.0 tokens; GLES 2.0 headers expose them
// only via extensions (gl2ext.h).  Provide the numeric literals as fallbacks so
// the code compiles against GLES 2.0 headers.  Values are from the OpenGL ES
// specification and are identical across all versions.
// GL_HALF_FLOAT_OES (0x8D61) is the correct type token for glTexImage2D on
// GLES 2.0; GL_HALF_FLOAT (0x140B) is its GLES 3.0 / desktop counterpart.
#ifndef GL_RGBA16F
#  define GL_RGBA16F 0x881A
#endif
#ifndef GL_HALF_FLOAT_OES
#  define GL_HALF_FLOAT_OES 0x8D61
#endif

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
  "  vec4 col = texture2D(u_tex, v_uv) * vec4(1.0, 1.0, 1.0, u_alpha);\n"
  "  vec3 rnd = fract(sin(vec3(dot(v_uv,vec2(127.1,311.7)),\n"
  "                           dot(v_uv,vec2(269.5,183.3)),\n"
  "                           dot(v_uv,vec2(419.2,371.9)))*43758.5453);\n"
  "  col.rgb += (rnd - 0.5) / 255.0;\n"
  "  gl_FragColor = col;\n"
  "}\n";
#else
  "#version 150\n"
  "in      vec2      v_uv;\n"
  "out     vec4      fragColor;\n"
  "uniform sampler2D u_tex;\n"
  "uniform float     u_alpha;\n"
  "void main() {\n"
  "  vec4 col = texture(u_tex, v_uv) * vec4(1.0, 1.0, 1.0, u_alpha);\n"
  "  vec3 rnd = fract(sin(vec3(dot(v_uv,vec2(127.1,311.7)),\n"
  "                           dot(v_uv,vec2(269.5,183.3)),\n"
  "                           dot(v_uv,vec2(419.2,371.9)))*43758.5453);\n"
  "  col.rgb += (rnd - 0.5) / 255.0;\n"
  "  fragColor = col;\n"
  "}\n";
#endif

// ── Solid colour shader (used for art shadow / glow) ─────────────────────────

static const char* SOLID_FRAG_SRC =
#if defined(HAS_GLES)
  "precision mediump float;\n"
  "uniform vec4 u_color;\n"
  "void main() { gl_FragColor = u_color; }\n";
#else
  "#version 150\n"
  "uniform vec4 u_color;\n"
  "out vec4 fragColor;\n"
  "void main() { fragColor = u_color; }\n";
#endif

// ── Separable Gaussian blur shader ───────────────────────────────────────────
// u_dir = (1/w, 0) for horizontal pass, (0, 1/h) for vertical pass
// 5-tap binomial kernel, step = 1.5 texels for wider spread

static const char* BLUR_FRAG_SRC =
#if defined(HAS_GLES)
  "precision mediump float;\n"
  "varying   vec2      v_uv;\n"
  "uniform   sampler2D u_tex;\n"
  "uniform   vec2      u_dir;\n"
  "void main() {\n"
  "  vec2 d = u_dir * 4.0;\n"
  "  vec4 c = texture2D(u_tex, v_uv - d*2.0) * 0.0625\n"
  "         + texture2D(u_tex, v_uv - d     ) * 0.25\n"
  "         + texture2D(u_tex, v_uv         ) * 0.375\n"
  "         + texture2D(u_tex, v_uv + d     ) * 0.25\n"
  "         + texture2D(u_tex, v_uv + d*2.0 ) * 0.0625;\n"
  "  vec3 rnd = fract(sin(vec3(dot(v_uv,vec2(127.1,311.7)),\n"
  "                           dot(v_uv,vec2(269.5,183.3)),\n"
  "                           dot(v_uv,vec2(419.2,371.9)))*43758.5453);\n"
  "  gl_FragColor = vec4(c.rgb*0.65+(rnd-0.5)/255.0, 1.0);\n"
  "}\n";
#else
  "#version 150\n"
  "in      vec2      v_uv;\n"
  "out     vec4      fragColor;\n"
  "uniform sampler2D u_tex;\n"
  "uniform vec2      u_dir;\n"
  "void main() {\n"
  "  vec2 d = u_dir * 4.0;\n"
  "  vec4 c = texture(u_tex, v_uv - d*2.0) * 0.0625\n"
  "         + texture(u_tex, v_uv - d     ) * 0.25\n"
  "         + texture(u_tex, v_uv         ) * 0.375\n"
  "         + texture(u_tex, v_uv + d     ) * 0.25\n"
  "         + texture(u_tex, v_uv + d*2.0 ) * 0.0625;\n"
  "  vec3 rnd = fract(sin(vec3(dot(v_uv,vec2(127.1,311.7)),\n"
  "                           dot(v_uv,vec2(269.5,183.3)),\n"
  "                           dot(v_uv,vec2(419.2,371.9)))*43758.5453);\n"
  "  fragColor = vec4(c.rgb*0.65+(rnd-0.5)/255.0, 1.0);\n"
  "}\n";
#endif

// ── BG shader 0: Audio Visualizer (chronos, Shadertoy CC0) ───────────────────

static const char* BG0_FRAG_SRC =
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

// ── BG shader 1: Metaballs ray-marcher ───────────────────────────────────────

static const char* BG1_FRAG_SRC =
#if defined(HAS_GLES)
  "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
  "precision highp float;\n"
  "#else\n"
  "precision mediump float;\n"
  "#endif\n"
  "uniform vec2  iResolution;\n"
  "uniform float iTime;\n"
  "uniform vec3  iHighlightColor;\n"
  "uniform vec3  iDarkColor;\n"
  "\n"
  "float opSmoothUnion(float d1,float d2,float k){\n"
  "  float h=clamp(0.5+0.5*(d2-d1)/k,0.0,1.0);\n"
  "  return mix(d2,d1,h)-k*h*(1.0-h);\n"
  "}\n"
  "float sdSphere(vec3 p,float s){return length(p)-s;}\n"
  "float mapScene(vec3 p){\n"
  "  float d=2.0;\n"
  "  for(int i=0;i<8;i++){\n"
  "    float fi=float(i);\n"
  "    float t=iTime*(fract(fi*412.531+0.513)-0.5)*2.0;\n"
  "    d=opSmoothUnion(\n"
  "      sdSphere(p+sin(t+fi*vec3(52.5126,64.62744,632.25))*vec3(2.0,2.0,0.8),\n"
  "               mix(0.5,1.0,fract(fi*412.531+0.5124))),d,0.4);\n"
  "  }\n"
  "  return d;\n"
  "}\n"
  "vec3 calcNormal(vec3 p){\n"
  "  float h=0.00001;\n"
  "  vec2 k=vec2(1.0,-1.0);\n"
  "  return normalize(k.xyy*mapScene(p+k.xyy*h)+k.yyx*mapScene(p+k.yyx*h)\n"
  "                  +k.yxy*mapScene(p+k.yxy*h)+k.xxx*mapScene(p+k.xxx*h));\n"
  "}\n"
  "void main(){\n"
  "  vec2 uv=gl_FragCoord.xy/iResolution.xy;\n"
  "  vec3 ro=vec3((uv-0.5)*vec2(iResolution.x/iResolution.y,1.0)*6.0,3.0);\n"
  "  ro.x-=1.2;\n"
  "  vec3 rd=vec3(0.0,0.0,-1.0);\n"
  "  float depth=0.0;\n"
  "  vec3 p=ro;\n"
  "  for(int i=0;i<32;i++){\n"
  "    p=ro+rd*depth;\n"
  "    float dist=mapScene(p);\n"
  "    depth+=dist;\n"
  "    if(dist<0.00001)break;\n"
  "  }\n"
  "  depth=min(6.0,depth);\n"
  "  vec3 n=calcNormal(p);\n"
  "  float b=max(0.0,dot(n,vec3(0.577)));\n"
  "  vec3 col=mix(iDarkColor,iHighlightColor,b*(0.85+b*0.35));\n"
  "  col*=exp(-depth*0.15);\n"
  "  gl_FragColor=vec4(col,1.0);\n"
  "}\n";
#else
  "#version 150\n"
  "uniform vec2  iResolution;\n"
  "uniform float iTime;\n"
  "uniform vec3  iHighlightColor;\n"
  "uniform vec3  iDarkColor;\n"
  "out vec4 fragColor;\n"
  "\n"
  "float opSmoothUnion(float d1,float d2,float k){\n"
  "  float h=clamp(0.5+0.5*(d2-d1)/k,0.0,1.0);\n"
  "  return mix(d2,d1,h)-k*h*(1.0-h);\n"
  "}\n"
  "float sdSphere(vec3 p,float s){return length(p)-s;}\n"
  "float mapScene(vec3 p){\n"
  "  float d=2.0;\n"
  "  for(int i=0;i<8;i++){\n"
  "    float fi=float(i);\n"
  "    float t=iTime*(fract(fi*412.531+0.513)-0.5)*2.0;\n"
  "    d=opSmoothUnion(\n"
  "      sdSphere(p+sin(t+fi*vec3(52.5126,64.62744,632.25))*vec3(2.0,2.0,0.8),\n"
  "               mix(0.5,1.0,fract(fi*412.531+0.5124))),d,0.4);\n"
  "  }\n"
  "  return d;\n"
  "}\n"
  "vec3 calcNormal(vec3 p){\n"
  "  float h=1e-5;\n"
  "  vec2 k=vec2(1.0,-1.0);\n"
  "  return normalize(k.xyy*mapScene(p+k.xyy*h)+k.yyx*mapScene(p+k.yyx*h)\n"
  "                  +k.yxy*mapScene(p+k.yxy*h)+k.xxx*mapScene(p+k.xxx*h));\n"
  "}\n"
  "void main(){\n"
  "  vec2 uv=gl_FragCoord.xy/iResolution.xy;\n"
  "  vec3 ro=vec3((uv-0.5)*vec2(iResolution.x/iResolution.y,1.0)*6.0,3.0);\n"
  "  ro.x-=1.2;\n"
  "  vec3 rd=vec3(0.0,0.0,-1.0);\n"
  "  float depth=0.0;\n"
  "  vec3 p=ro;\n"
  "  for(int i=0;i<32;i++){\n"
  "    p=ro+rd*depth;\n"
  "    float dist=mapScene(p);\n"
  "    depth+=dist;\n"
  "    if(dist<1e-6)break;\n"
  "  }\n"
  "  depth=min(6.0,depth);\n"
  "  vec3 n=calcNormal(p);\n"
  "  float b=max(0.0,dot(n,vec3(0.577)));\n"
  "  vec3 col=mix(iDarkColor,iHighlightColor,b*(0.85+b*0.35));\n"
  "  col*=exp(-depth*0.15);\n"
  "  fragColor=vec4(col,1.0);\n"
  "}\n";
#endif

// ── BG shader 2: Nebula Ring ─────────────────────────────────────────────────

static const char* BG2_FRAG_SRC =
#if defined(HAS_GLES)
  "precision mediump float;\n"
  "uniform vec2  iResolution;\n"
  "uniform float iTime;\n"
  "\n"
  "vec3 hash33(vec3 p3) {\n"
  "  p3 = fract(p3 * vec3(0.1031,0.11369,0.13787));\n"
  "  p3 += dot(p3, p3.yxz + 19.19);\n"
  "  return -1.0 + 2.0*fract(vec3(p3.x+p3.y,p3.x+p3.z,p3.y+p3.z)*p3.zyx);\n"
  "}\n"
  "float snoise3(vec3 p) {\n"
  "  const float K1=0.333333333; const float K2=0.166666667;\n"
  "  vec3 i=floor(p+(p.x+p.y+p.z)*K1);\n"
  "  vec3 d0=p-(i-(i.x+i.y+i.z)*K2);\n"
  "  vec3 e=step(vec3(0.0),d0-d0.yzx);\n"
  "  vec3 i1=e*(1.0-e.zxy); vec3 i2=1.0-e.zxy*(1.0-e);\n"
  "  vec3 d1=d0-(i1-K2); vec3 d2=d0-(i2-K1); vec3 d3=d0-0.5;\n"
  "  vec4 h=max(0.6-vec4(dot(d0,d0),dot(d1,d1),dot(d2,d2),dot(d3,d3)),0.0);\n"
  "  vec4 n=h*h*h*h*vec4(dot(d0,hash33(i)),dot(d1,hash33(i+i1)),\n"
  "                       dot(d2,hash33(i+i2)),dot(d3,hash33(i+1.0)));\n"
  "  return dot(vec4(31.316),n);\n"
  "}\n"
  "vec4 extractAlpha(vec3 c) {\n"
  "  float m=min(max(max(c.r,c.g),c.b),1.0);\n"
  "  return m>1e-5 ? vec4(c/m,m) : vec4(0.0);\n"
  "}\n"
  "const vec3 col1=vec3(0.611765,0.262745,0.996078);\n"
  "const vec3 col2=vec3(0.298039,0.760784,0.913725);\n"
  "const vec3 col3=vec3(0.062745,0.078431,0.600000);\n"
  "const float IR=0.6; const float NS=0.65;\n"
  "float L1(float i,float a,float d){return i/(1.0+d*a);}\n"
  "float L2(float i,float a,float d){return i/(1.0+d*d*a);}\n"
  "void draw(out vec4 fc, in vec2 uv) {\n"
  "  float ang=atan(uv.y,uv.x); float len=length(uv);\n"
  "  float v0,v1,v2,v3,cl,r0,d0,n0,d;\n"
  "  n0=snoise3(vec3(uv*NS,iTime*0.5))*0.5+0.5;\n"
  "  r0=mix(mix(IR,1.0,0.4),mix(IR,1.0,0.6),n0);\n"
  "  d0=distance(uv,r0/len*uv);\n"
  "  v0=L1(1.0,10.0,d0)*smoothstep(r0*1.05,r0,len);\n"
  "  cl=cos(ang+iTime*2.0)*0.5+0.5;\n"
  "  vec2 pos=vec2(cos(-iTime),sin(-iTime))*r0;\n"
  "  d=distance(uv,pos);\n"
  "  v1=L2(1.5,5.0,d)*L1(1.0,50.0,d0);\n"
  "  v2=smoothstep(1.0,mix(IR,1.0,n0*0.5),len);\n"
  "  v3=smoothstep(IR,mix(IR,1.0,0.5),len);\n"
  "  vec3 col=mix(col1,col2,cl);\n"
  "  col=mix(col3,col,v0);\n"
  "  col=clamp((col+v1)*v2*v3,0.0,1.0);\n"
  "  fc=extractAlpha(col);\n"
  "}\n"
  "void main() {\n"
  "  vec2 uv=(gl_FragCoord.xy*2.0-iResolution.xy)/iResolution.y;\n"
  "  vec4 col; draw(col,uv);\n"
  "  gl_FragColor=vec4(mix(vec3(0.0),col.rgb,col.a),1.0);\n"
  "}\n";
#else
  "#version 150\n"
  "uniform vec2  iResolution;\n"
  "uniform float iTime;\n"
  "out vec4 fragColor;\n"
  "\n"
  "vec3 hash33(vec3 p3) {\n"
  "  p3 = fract(p3 * vec3(0.1031,0.11369,0.13787));\n"
  "  p3 += dot(p3, p3.yxz + 19.19);\n"
  "  return -1.0 + 2.0*fract(vec3(p3.x+p3.y,p3.x+p3.z,p3.y+p3.z)*p3.zyx);\n"
  "}\n"
  "float snoise3(vec3 p) {\n"
  "  const float K1=0.333333333; const float K2=0.166666667;\n"
  "  vec3 i=floor(p+(p.x+p.y+p.z)*K1);\n"
  "  vec3 d0=p-(i-(i.x+i.y+i.z)*K2);\n"
  "  vec3 e=step(vec3(0.0),d0-d0.yzx);\n"
  "  vec3 i1=e*(1.0-e.zxy); vec3 i2=1.0-e.zxy*(1.0-e);\n"
  "  vec3 d1=d0-(i1-K2); vec3 d2=d0-(i2-K1); vec3 d3=d0-0.5;\n"
  "  vec4 h=max(0.6-vec4(dot(d0,d0),dot(d1,d1),dot(d2,d2),dot(d3,d3)),0.0);\n"
  "  vec4 n=h*h*h*h*vec4(dot(d0,hash33(i)),dot(d1,hash33(i+i1)),\n"
  "                       dot(d2,hash33(i+i2)),dot(d3,hash33(i+1.0)));\n"
  "  return dot(vec4(31.316),n);\n"
  "}\n"
  "vec4 extractAlpha(vec3 c) {\n"
  "  float m=min(max(max(c.r,c.g),c.b),1.0);\n"
  "  return m>1e-5 ? vec4(c/m,m) : vec4(0.0);\n"
  "}\n"
  "const vec3 col1=vec3(0.611765,0.262745,0.996078);\n"
  "const vec3 col2=vec3(0.298039,0.760784,0.913725);\n"
  "const vec3 col3=vec3(0.062745,0.078431,0.600000);\n"
  "const float IR=0.6; const float NS=0.65;\n"
  "float L1(float i,float a,float d){return i/(1.0+d*a);}\n"
  "float L2(float i,float a,float d){return i/(1.0+d*d*a);}\n"
  "void draw(out vec4 fc, in vec2 uv) {\n"
  "  float ang=atan(uv.y,uv.x); float len=length(uv);\n"
  "  float v0,v1,v2,v3,cl,r0,d0,n0,d;\n"
  "  n0=snoise3(vec3(uv*NS,iTime*0.5))*0.5+0.5;\n"
  "  r0=mix(mix(IR,1.0,0.4),mix(IR,1.0,0.6),n0);\n"
  "  d0=distance(uv,r0/len*uv);\n"
  "  v0=L1(1.0,10.0,d0)*smoothstep(r0*1.05,r0,len);\n"
  "  cl=cos(ang+iTime*2.0)*0.5+0.5;\n"
  "  vec2 pos=vec2(cos(-iTime),sin(-iTime))*r0;\n"
  "  d=distance(uv,pos);\n"
  "  v1=L2(1.5,5.0,d)*L1(1.0,50.0,d0);\n"
  "  v2=smoothstep(1.0,mix(IR,1.0,n0*0.5),len);\n"
  "  v3=smoothstep(IR,mix(IR,1.0,0.5),len);\n"
  "  vec3 col=mix(col1,col2,cl);\n"
  "  col=mix(col3,col,v0);\n"
  "  col=clamp((col+v1)*v2*v3,0.0,1.0);\n"
  "  fc=extractAlpha(col);\n"
  "}\n"
  "void main() {\n"
  "  vec2 uv=(gl_FragCoord.xy*2.0-iResolution.xy)/iResolution.y;\n"
  "  vec4 col; draw(col,uv);\n"
  "  fragColor=vec4(mix(vec3(0.0),col.rgb,col.a),1.0);\n"
  "}\n";
#endif

// ── BG shader 3: Wavy Lines (audio-reactive via iChannel0) ───────────────────
// Audio texture: row 0 (y=0) = FFT frequency, row 1 (y=1) = waveform

static const char* BG3_FRAG_SRC =
#if defined(HAS_GLES)
  "precision mediump float;\n"
  "uniform vec2      iResolution;\n"
  "uniform float     iTime;\n"
  "uniform sampler2D iChannel0;\n"
  "\n"
  "float squared(float v){return v*v;}\n"
  "float getAmp(float f){return texture2D(iChannel0,vec2(f/512.0,0.0)).x;}\n"
  "float getWeight(float f){\n"
  "  return(getAmp(f-1.0)+getAmp(f)+getAmp(f+1.0))/3.0;\n"
  "}\n"
  "void main() {\n"
  "  float t=mod(iTime,100.0);\n"  // keep argument small — mediump loses precision above ~100
  "  vec2 uvT=gl_FragCoord.xy/iResolution.xy;\n"
  "  vec2 uv=-1.0+2.0*uvT;\n"
  "  float li,gw;\n"
  "  vec3 color=vec3(0.0);\n"
  "  for(float i=0.0;i<5.0;i++){\n"
  "    uv.y+=0.2*sin(uv.x+i/7.0-t*0.6);\n"
  "    float Y=uv.y+getWeight(squared(i)*20.0)\n"
  "            *(texture2D(iChannel0,vec2(uvT.x,1.0)).x-0.5);\n"
  "    li=0.4+squared(1.6*abs(mod(uvT.x+i/1.3+t,2.0)-1.0));\n"
  "    gw=abs(li/(150.0*max(abs(Y),0.001)));\n"  // clamp Y away from zero — prevents Inf/NaN
  "    color+=vec3(gw*(2.0+sin(t*0.13)),\n"
  "                gw*(2.0-sin(t*0.23)),\n"
  "                gw*(2.0-cos(t*0.19)));\n"
  "  }\n"
  "  gl_FragColor=vec4(min(color,vec3(8.0)),1.0);\n"  // clamp runaway gw before output
  "}\n";
#else
  "#version 150\n"
  "uniform vec2      iResolution;\n"
  "uniform float     iTime;\n"
  "uniform sampler2D iChannel0;\n"
  "out vec4 fragColor;\n"
  "\n"
  "float squared(float v){return v*v;}\n"
  "float getAmp(float f){return texture(iChannel0,vec2(f/512.0,0.0)).x;}\n"
  "float getWeight(float f){\n"
  "  return(getAmp(f-1.0)+getAmp(f)+getAmp(f+1.0))/3.0;\n"
  "}\n"
  "void main() {\n"
  "  float t=mod(iTime,100.0);\n"
  "  vec2 uvT=gl_FragCoord.xy/iResolution.xy;\n"
  "  vec2 uv=-1.0+2.0*uvT;\n"
  "  float li,gw;\n"
  "  vec3 color=vec3(0.0);\n"
  "  for(float i=0.0;i<5.0;i++){\n"
  "    uv.y+=0.2*sin(uv.x+i/7.0-t*0.6);\n"
  "    float Y=uv.y+getWeight(squared(i)*20.0)\n"
  "            *(texture(iChannel0,vec2(uvT.x,1.0)).x-0.5);\n"
  "    li=0.4+squared(1.6*abs(mod(uvT.x+i/1.3+t,2.0)-1.0));\n"
  "    gw=abs(li/(150.0*max(abs(Y),0.001)));\n"
  "    color+=vec3(gw*(2.0+sin(t*0.13)),\n"
  "                gw*(2.0-sin(t*0.23)),\n"
  "                gw*(2.0-cos(t*0.19)));\n"
  "  }\n"
  "  fragColor=vec4(min(color,vec3(8.0)),1.0);\n"
  "}\n";
#endif

// ── BG shader 4: Tunnelwisp Lite ─────────────────────────────────────────────
// Lightweight fake tunnel using polar coords + 2D sin/cos pattern.
// No loops, no raymarching — runs on low-end devices.

static const char* BG4_FRAG_SRC =
#if defined(HAS_GLES)
  "precision mediump float;\n"
  "uniform vec2  iResolution;\n"
  "uniform float iTime;\n"
  "void main(){\n"
  "  vec2 uv=(gl_FragCoord.xy*2.0-iResolution.xy)/iResolution.y;\n"
  "  float r=length(uv)+1e-4;\n"
  "  float a=atan(uv.y,uv.x);\n"
  "  float T=iTime*0.4;\n"
  "  float tx=a/3.14159265;\n"
  "  float ty=1.0/r+T;\n"
  "  tx+=sin(ty*1.5+T*0.5)*0.25;\n"
  "  float p=sin(tx*9.42)*cos(ty*6.28)\n"
  "         +cos(tx*6.28+T)*sin(ty*9.42+T*0.7);\n"
  "  p=p*0.5+0.5;\n"
  "  vec3 col=0.5+0.5*cos(vec3(p*4.0,p*4.0+2.094,p*4.0+4.189)+T*0.3);\n"
  "  float glow=0.15/(r+0.05);\n"
  "  col=col*min(glow,1.5)*(0.6+0.4*p);\n"
  "  col=col/(col+1.0);\n"
  "  gl_FragColor=vec4(col,1.0);\n"
  "}\n";
#else
  "#version 150\n"
  "uniform vec2  iResolution;\n"
  "uniform float iTime;\n"
  "out vec4 fragColor;\n"
  "void main(){\n"
  "  vec2 uv=(gl_FragCoord.xy*2.0-iResolution.xy)/iResolution.y;\n"
  "  float r=length(uv)+1e-4;\n"
  "  float a=atan(uv.y,uv.x);\n"
  "  float T=iTime*0.4;\n"
  "  float tx=a/3.14159265;\n"
  "  float ty=1.0/r+T;\n"
  "  tx+=sin(ty*1.5+T*0.5)*0.25;\n"
  "  float p=sin(tx*9.42)*cos(ty*6.28)\n"
  "         +cos(tx*6.28+T)*sin(ty*9.42+T*0.7);\n"
  "  p=p*0.5+0.5;\n"
  "  vec3 col=0.5+0.5*cos(vec3(p*4.0,p*4.0+2.094,p*4.0+4.189)+T*0.3);\n"
  "  float glow=0.15/(r+0.05);\n"
  "  col=col*min(glow,1.5)*(0.6+0.4*p);\n"
  "  col=col/(col+1.0);\n"
  "  fragColor=vec4(col,1.0);\n"
  "}\n";
#endif

// ── BG shader 6: Plasma (audio-reactive Lissajous / colour plasma) ───────────
// iChannel0 = audio texture (freq row y=0). iMouse omitted (no cursor in Kodi).

static const char* BG6_FRAG_SRC =
#if defined(HAS_GLES)
  "precision mediump float;\n"
  "uniform vec2      iResolution;\n"
  "uniform float     iTime;\n"
  "uniform sampler2D iChannel0;\n"
  "#define pi 3.14159\n"
  "const vec2 vp=vec2(320.0,200.0);\n"
  "void main(){\n"
  "  float Freq=texture2D(iChannel0,vec2(0.0,0.0)).r;\n"
  "  float t=iTime*10.0+Freq*80.0;\n"
  "  vec2 uv=(gl_FragCoord.xy-iResolution.xy*0.5)/iResolution.xy*(0.7+Freq*0.3);\n"
  "  float R=cos(Freq*5.0)*0.2;\n"
  "  uv*=mat2(cos(R),-sin(R),sin(R),cos(R));\n"
  "  uv+=iTime*0.3;\n"
  "  vec2 p0=(uv-0.5)*vp;\n"
  "  vec2 hvp=vp*0.5;\n"
  "  vec2 p1d=vec2(cos(t/98.0),sin(t/178.0))*hvp-p0;\n"
  "  vec2 p2d=vec2(sin(-t/124.0),cos(-t/104.0))*hvp-p0;\n"
  "  vec2 p3d=vec2(cos(-t/165.0),cos(t/45.0))*hvp-p0;\n"
  "  float sum=0.5+0.5*(cos(length(p1d)/40.0)+cos(length(p2d)/30.0)\n"
  "    +sin(length(p3d)/35.0)*sin(p3d.x/20.0)*sin(p3d.y/15.0));\n"
  "  vec3 Color=vec3(\n"
  "    cos(Freq+uv.x*3.0+iTime+pi*0.333333)*0.5+0.5,\n"
  "    cos(Freq+uv.y*3.0+iTime+pi*0.666666)*0.5+0.5,\n"
  "    -cos(Freq+length(uv)*3.0+iTime)*0.5+0.5);\n"
  "  gl_FragColor=vec4(Color*texture2D(iChannel0,vec2(fract(sum+iTime),0.0)).r,1.0);\n"
  "}\n";
#else
  "#version 150\n"
  "uniform vec2      iResolution;\n"
  "uniform float     iTime;\n"
  "uniform sampler2D iChannel0;\n"
  "out vec4 fragColor;\n"
  "#define pi 3.14159\n"
  "const vec2 vp=vec2(320.0,200.0);\n"
  "void main(){\n"
  "  float Freq=texture(iChannel0,vec2(0.0,0.0)).r;\n"
  "  float t=iTime*10.0+Freq*80.0;\n"
  "  vec2 uv=(gl_FragCoord.xy-iResolution.xy*0.5)/iResolution.xy*(0.7+Freq*0.3);\n"
  "  float R=cos(Freq*5.0)*0.2;\n"
  "  uv*=mat2(cos(R),-sin(R),sin(R),cos(R));\n"
  "  uv+=iTime*0.3;\n"
  "  vec2 p0=(uv-0.5)*vp;\n"
  "  vec2 hvp=vp*0.5;\n"
  "  vec2 p1d=vec2(cos(t/98.0),sin(t/178.0))*hvp-p0;\n"
  "  vec2 p2d=vec2(sin(-t/124.0),cos(-t/104.0))*hvp-p0;\n"
  "  vec2 p3d=vec2(cos(-t/165.0),cos(t/45.0))*hvp-p0;\n"
  "  float sum=0.5+0.5*(cos(length(p1d)/40.0)+cos(length(p2d)/30.0)\n"
  "    +sin(length(p3d)/35.0)*sin(p3d.x/20.0)*sin(p3d.y/15.0));\n"
  "  vec3 Color=vec3(\n"
  "    cos(Freq+uv.x*3.0+iTime+pi*0.333333)*0.5+0.5,\n"
  "    cos(Freq+uv.y*3.0+iTime+pi*0.666666)*0.5+0.5,\n"
  "    -cos(Freq+length(uv)*3.0+iTime)*0.5+0.5);\n"
  "  fragColor=vec4(Color*texture(iChannel0,vec2(fract(sum+iTime),0.0)).r,1.0);\n"
  "}\n";
#endif

// ── BG shader 7: Spectrum analyser bars (audio-reactive) ─────────────────────
// 48 rounded capsule bars, log frequency mapping, bloom, screen blend.
// No iTime — purely audio-reactive.

static const char* BG7_FRAG_SRC =
#if defined(HAS_GLES)
  "precision mediump float;\n"
  "uniform vec2      iResolution;\n"
  "uniform sampler2D iChannel0;\n"
  "#define BAR_WIDTH 0.008\n"
  "#define BAR_SPACING 0.014\n"
  "#define MAX_BAR_HEIGHT 0.7\n"
  "#define BLOOM_SIZE 12.0\n"
  "#define BLOOM_INTENSITY 0.3\n"
  "#define BLOOM_FALLOFF 2.0\n"
  "#define COLOR_SCHEME 0\n"
  "#define SINGLE_COLOR vec3(1.0,1.0,1.0)\n"
  "#define LOG_SCALE_FACTOR 0.5\n"
  "#define CENTER_LINE_THICKNESS 0.006\n"
  "#define CENTER_LINE_OVERHANG 0.05\n"
  "#define CENTER_LINE_COLOR vec3(0.6,0.6,0.6)\n"
  "#define NUM_BARS 48\n"
  "#define CENTER_Y 0.5\n"
  "#define WAVEFORM_WIDTH 0.8\n"
  "#define COLOR_LOW  vec3(1.0,0.2,0.1)\n"
  "#define COLOR_MID  vec3(1.0,1.0,0.2)\n"
  "#define COLOR_HIGH vec3(0.2,0.4,1.0)\n"
  "float getFreqPos(int i){\n"
  "  float t=float(i)/float(NUM_BARS-1);\n"
  "  return sqrt(t);\n"
  "}\n"
  "float getFreq(int i){\n"
  "  return texture2D(iChannel0,vec2(clamp(getFreqPos(i),0.0,1.0),0.25)).x;\n"
  "}\n"
  "vec3 hsv2rgb(vec3 c){\n"
  "  vec4 K=vec4(1.0,2.0/3.0,1.0/3.0,3.0);\n"
  "  vec3 p=abs(fract(c.xxx+K.xyz)*6.0-K.www);\n"
  "  return c.z*mix(K.xxx,clamp(p-K.xxx,0.0,1.0),c.y);\n"
  "}\n"
  "vec3 getColor(int i){\n"
  "  float t=float(i)/float(NUM_BARS-1);\n"
  "  if(COLOR_SCHEME==1)return SINGLE_COLOR;\n"
  "  if(COLOR_SCHEME==2)return hsv2rgb(vec3(t*0.8,1.0,1.0));\n"
  "  if(COLOR_SCHEME==3)return mix(vec3(0.3,0.1,0.5),vec3(0.8,0.4,1.0),t);\n"
  "  if(COLOR_SCHEME==4)return mix(vec3(0.1,0.3,0.1),vec3(0.4,1.0,0.4),t);\n"
  "  if(COLOR_SCHEME==5)return mix(vec3(0.1,0.2,0.5),vec3(0.4,0.8,1.0),t);\n"
  "  return t<0.5?mix(COLOR_LOW,COLOR_MID,t*2.0):mix(COLOR_MID,COLOR_HIGH,(t-0.5)*2.0);\n"
  "}\n"
  "float distBar(vec2 uv,float cx,float w,float h){\n"
  "  vec2 p=uv-vec2(cx,CENTER_Y);\n"
  "  float r=w*0.5;\n"
  "  float hh=max(h*0.5,r);\n"
  "  vec2 q=p-vec2(0.0,clamp(p.y,-(hh-r),hh-r));\n"
  "  return length(q)-r;\n"
  "}\n"
  "float distLine(vec2 uv,float x0,float x1){\n"
  "  return length(uv-vec2(clamp(uv.x,x0,x1),CENTER_Y));\n"
  "}\n"
  "void main(){\n"
  "  vec2 uv=gl_FragCoord.xy/iResolution.xy;\n"
  "  vec3 col=vec3(0.0);\n"
  "  vec2 px=1.0/iResolution.xy;\n"
  "  float sp=min(BAR_SPACING,(WAVEFORM_WIDTH-float(NUM_BARS)*BAR_WIDTH)/float(NUM_BARS-1));\n"
  "  float ww=float(NUM_BARS)*BAR_WIDTH+float(NUM_BARS-1)*sp;\n"
  "  float sx=0.5-ww*0.5;\n"
  "  float lx0=max(sx-CENTER_LINE_OVERHANG*WAVEFORM_WIDTH,0.0);\n"
  "  float lx1=min(sx+ww+CENTER_LINE_OVERHANG*WAVEFORM_WIDTH,1.0);\n"
  "  float cd=distLine(uv,lx0,lx1);\n"
  "  col+=CENTER_LINE_COLOR*(1.0-smoothstep(0.0,CENTER_LINE_THICKNESS,cd)\n"
  "    +exp(-cd*BLOOM_FALLOFF/(BLOOM_SIZE*0.5*length(px)))*BLOOM_INTENSITY*0.3);\n"
  "  for(int i=0;i<NUM_BARS;i++){\n"
  "    float freq=getFreq(i);\n"
  "    vec3 bc=getColor(i);\n"
  "    float bx=sx+float(i)*(BAR_WIDTH+sp)+BAR_WIDTH*0.5;\n"
  "    float d=distBar(uv,bx,BAR_WIDTH,max(freq*MAX_BAR_HEIGHT,BAR_WIDTH));\n"
  "    float gs=BLOOM_SIZE*length(px);\n"
  "    vec3 lc=bc*((1.0-smoothstep(-px.x,px.x,d))\n"
  "      +exp(-max(d,0.0)*BLOOM_FALLOFF/gs)*BLOOM_INTENSITY\n"
  "      +exp(-max(d,0.0)*(BLOOM_FALLOFF*0.5)/(gs*2.0))*(BLOOM_INTENSITY*0.5));\n"
  "    col=col+lc-col*lc;\n"
  "  }\n"
  "  gl_FragColor=vec4(col,1.0);\n"
  "}\n";
#else
  "#version 150\n"
  "uniform vec2      iResolution;\n"
  "uniform sampler2D iChannel0;\n"
  "out vec4 fragColor;\n"
  "#define BAR_WIDTH 0.008\n"
  "#define BAR_SPACING 0.014\n"
  "#define MAX_BAR_HEIGHT 0.7\n"
  "#define BLOOM_SIZE 12.0\n"
  "#define BLOOM_INTENSITY 0.3\n"
  "#define BLOOM_FALLOFF 2.0\n"
  "#define COLOR_SCHEME 0\n"
  "#define SINGLE_COLOR vec3(1.0,1.0,1.0)\n"
  "#define LOG_SCALE_FACTOR 0.5\n"
  "#define CENTER_LINE_THICKNESS 0.006\n"
  "#define CENTER_LINE_OVERHANG 0.05\n"
  "#define CENTER_LINE_COLOR vec3(0.6,0.6,0.6)\n"
  "#define NUM_BARS 48\n"
  "#define CENTER_Y 0.5\n"
  "#define WAVEFORM_WIDTH 0.8\n"
  "#define COLOR_LOW  vec3(1.0,0.2,0.1)\n"
  "#define COLOR_MID  vec3(1.0,1.0,0.2)\n"
  "#define COLOR_HIGH vec3(0.2,0.4,1.0)\n"
  "float getFreqPos(int i){\n"
  "  float t=float(i)/float(NUM_BARS-1);\n"
  "  return pow(t,LOG_SCALE_FACTOR);\n"
  "}\n"
  "float getFreq(int i){\n"
  "  return texture(iChannel0,vec2(clamp(getFreqPos(i),0.0,1.0),0.25)).x;\n"
  "}\n"
  "vec3 hsv2rgb(vec3 c){\n"
  "  vec4 K=vec4(1.0,2.0/3.0,1.0/3.0,3.0);\n"
  "  vec3 p=abs(fract(c.xxx+K.xyz)*6.0-K.www);\n"
  "  return c.z*mix(K.xxx,clamp(p-K.xxx,0.0,1.0),c.y);\n"
  "}\n"
  "vec3 getColor(int i){\n"
  "  float t=float(i)/float(NUM_BARS-1);\n"
  "  if(COLOR_SCHEME==1)return SINGLE_COLOR;\n"
  "  if(COLOR_SCHEME==2)return hsv2rgb(vec3(t*0.8,1.0,1.0));\n"
  "  if(COLOR_SCHEME==3)return mix(vec3(0.3,0.1,0.5),vec3(0.8,0.4,1.0),t);\n"
  "  if(COLOR_SCHEME==4)return mix(vec3(0.1,0.3,0.1),vec3(0.4,1.0,0.4),t);\n"
  "  if(COLOR_SCHEME==5)return mix(vec3(0.1,0.2,0.5),vec3(0.4,0.8,1.0),t);\n"
  "  return t<0.5?mix(COLOR_LOW,COLOR_MID,t*2.0):mix(COLOR_MID,COLOR_HIGH,(t-0.5)*2.0);\n"
  "}\n"
  "float distBar(vec2 uv,float cx,float w,float h){\n"
  "  vec2 p=uv-vec2(cx,CENTER_Y);\n"
  "  float r=w*0.5;\n"
  "  float hh=max(h*0.5,r);\n"
  "  vec2 q=p-vec2(0.0,clamp(p.y,-(hh-r),hh-r));\n"
  "  return length(q)-r;\n"
  "}\n"
  "float distLine(vec2 uv,float x0,float x1){\n"
  "  return length(uv-vec2(clamp(uv.x,x0,x1),CENTER_Y));\n"
  "}\n"
  "void main(){\n"
  "  vec2 uv=gl_FragCoord.xy/iResolution.xy;\n"
  "  vec3 col=vec3(0.0);\n"
  "  vec2 px=1.0/iResolution.xy;\n"
  "  float sp=min(BAR_SPACING,(WAVEFORM_WIDTH-float(NUM_BARS)*BAR_WIDTH)/float(NUM_BARS-1));\n"
  "  float ww=float(NUM_BARS)*BAR_WIDTH+float(NUM_BARS-1)*sp;\n"
  "  float sx=0.5-ww*0.5;\n"
  "  float lx0=max(sx-CENTER_LINE_OVERHANG*WAVEFORM_WIDTH,0.0);\n"
  "  float lx1=min(sx+ww+CENTER_LINE_OVERHANG*WAVEFORM_WIDTH,1.0);\n"
  "  float cd=distLine(uv,lx0,lx1);\n"
  "  col+=CENTER_LINE_COLOR*(1.0-smoothstep(0.0,CENTER_LINE_THICKNESS,cd)\n"
  "    +exp(-cd*BLOOM_FALLOFF/(BLOOM_SIZE*0.5*length(px)))*BLOOM_INTENSITY*0.3);\n"
  "  for(int i=0;i<NUM_BARS;i++){\n"
  "    float freq=getFreq(i);\n"
  "    vec3 bc=getColor(i);\n"
  "    float bx=sx+float(i)*(BAR_WIDTH+sp)+BAR_WIDTH*0.5;\n"
  "    float d=distBar(uv,bx,BAR_WIDTH,max(freq*MAX_BAR_HEIGHT,BAR_WIDTH));\n"
  "    float gs=BLOOM_SIZE*length(px);\n"
  "    vec3 lc=bc*((1.0-smoothstep(-px.x,px.x,d))\n"
  "      +exp(-max(d,0.0)*BLOOM_FALLOFF/gs)*BLOOM_INTENSITY\n"
  "      +exp(-max(d,0.0)*(BLOOM_FALLOFF*0.5)/(gs*2.0))*(BLOOM_INTENSITY*0.5));\n"
  "    col=col+lc-col*lc;\n"
  "  }\n"
  "  fragColor=vec4(col,1.0);\n"
  "}\n";
#endif

// ── BG shader 8: Metaballs Chroma (album-art colour, 30% slower) ─────────────
// Same ray-marcher as BG1 but speed scaled to 0.7× and coloured by the
// dominant hue extracted from the current album art.

static const char* BG8_FRAG_SRC =
#if defined(HAS_GLES)
  "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
  "precision highp float;\n"
  "#else\n"
  "precision mediump float;\n"
  "#endif\n"
  "uniform vec2  iResolution;\n"
  "uniform float iTime;\n"
  "uniform vec3  iHighlightColor;\n"
  "uniform vec3  iDarkColor;\n"
  "\n"
  "float opSmoothUnion(float d1,float d2,float k){\n"
  "  float h=clamp(0.5+0.5*(d2-d1)/k,0.0,1.0);\n"
  "  return mix(d2,d1,h)-k*h*(1.0-h);\n"
  "}\n"
  "float sdSphere(vec3 p,float s){return length(p)-s;}\n"
  "float mapScene(vec3 p,float t){\n"
  "  float d=2.0;\n"
  "  for(int i=0;i<8;i++){\n"
  "    float fi=float(i);\n"
  "    float ts=t*(fract(fi*412.531+0.513)-0.5)*2.0;\n"
  "    d=opSmoothUnion(\n"
  "      sdSphere(p+sin(ts+fi*vec3(52.5126,64.62744,632.25))*vec3(2.0,2.0,0.8),\n"
  "               mix(0.5,1.0,fract(fi*412.531+0.5124))),d,0.4);\n"
  "  }\n"
  "  return d;\n"
  "}\n"
  "vec3 calcNormal(vec3 p,float t){\n"
  "  float h=0.00001;\n"
  "  vec2 k=vec2(1.0,-1.0);\n"
  "  return normalize(k.xyy*mapScene(p+k.xyy*h,t)+k.yyx*mapScene(p+k.yyx*h,t)\n"
  "                  +k.yxy*mapScene(p+k.yxy*h,t)+k.xxx*mapScene(p+k.xxx*h,t));\n"
  "}\n"
  "void main(){\n"
  "  float t=mod(iTime*0.7,100.0);\n"  // 30% slower; wrapped for mediump precision
  "  vec2 uv=gl_FragCoord.xy/iResolution.xy;\n"
  "  vec3 ro=vec3((uv-0.5)*vec2(iResolution.x/iResolution.y,1.0)*6.0,3.0);\n"
  "  ro.x-=1.2;\n"
  "  vec3 rd=vec3(0.0,0.0,-1.0);\n"
  "  float depth=0.0;\n"
  "  vec3 p=ro;\n"
  "  for(int i=0;i<32;i++){\n"
  "    p=ro+rd*depth;\n"
  "    float dist=mapScene(p,t);\n"
  "    depth+=dist;\n"
  "    if(dist<0.00001)break;\n"
  "  }\n"
  "  depth=min(6.0,depth);\n"
  "  vec3 n=calcNormal(p,t);\n"
  "  float b=max(0.0,dot(n,vec3(0.577)));\n"
  "  vec3 col=mix(iDarkColor,iHighlightColor,b*(0.85+b*0.35));\n"
  "  col*=exp(-depth*0.15);\n"
  "  gl_FragColor=vec4(col,1.0);\n"
  "}\n";
#else
  "#version 150\n"
  "uniform vec2  iResolution;\n"
  "uniform float iTime;\n"
  "uniform vec3  iHighlightColor;\n"
  "uniform vec3  iDarkColor;\n"
  "out vec4 fragColor;\n"
  "\n"
  "float opSmoothUnion(float d1,float d2,float k){\n"
  "  float h=clamp(0.5+0.5*(d2-d1)/k,0.0,1.0);\n"
  "  return mix(d2,d1,h)-k*h*(1.0-h);\n"
  "}\n"
  "float sdSphere(vec3 p,float s){return length(p)-s;}\n"
  "float mapScene(vec3 p,float t){\n"
  "  float d=2.0;\n"
  "  for(int i=0;i<8;i++){\n"
  "    float fi=float(i);\n"
  "    float ts=t*(fract(fi*412.531+0.513)-0.5)*2.0;\n"
  "    d=opSmoothUnion(\n"
  "      sdSphere(p+sin(ts+fi*vec3(52.5126,64.62744,632.25))*vec3(2.0,2.0,0.8),\n"
  "               mix(0.5,1.0,fract(fi*412.531+0.5124))),d,0.4);\n"
  "  }\n"
  "  return d;\n"
  "}\n"
  "vec3 calcNormal(vec3 p,float t){\n"
  "  float h=1e-5;\n"
  "  vec2 k=vec2(1.0,-1.0);\n"
  "  return normalize(k.xyy*mapScene(p+k.xyy*h,t)+k.yyx*mapScene(p+k.yyx*h,t)\n"
  "                  +k.yxy*mapScene(p+k.yxy*h,t)+k.xxx*mapScene(p+k.xxx*h,t));\n"
  "}\n"
  "void main(){\n"
  "  float t=mod(iTime*0.7,100.0);\n"
  "  vec2 uv=gl_FragCoord.xy/iResolution.xy;\n"
  "  vec3 ro=vec3((uv-0.5)*vec2(iResolution.x/iResolution.y,1.0)*6.0,3.0);\n"
  "  ro.x-=1.2;\n"
  "  vec3 rd=vec3(0.0,0.0,-1.0);\n"
  "  float depth=0.0;\n"
  "  vec3 p=ro;\n"
  "  for(int i=0;i<32;i++){\n"
  "    p=ro+rd*depth;\n"
  "    float dist=mapScene(p,t);\n"
  "    depth+=dist;\n"
  "    if(dist<1e-6)break;\n"
  "  }\n"
  "  depth=min(6.0,depth);\n"
  "  vec3 n=calcNormal(p,t);\n"
  "  float b=max(0.0,dot(n,vec3(0.577)));\n"
  "  vec3 col=mix(iDarkColor,iHighlightColor,b*(0.85+b*0.35));\n"
  "  col*=exp(-depth*0.15);\n"
  "  fragColor=vec4(col,1.0);\n"
  "}\n";
#endif

// ── FFT (Cooley-Tukey radix-2) ────────────────────────────────────────────────

static void ComputeFFTMagnitudes(const float* mono, int n, float* out,
                                  std::vector<std::complex<float>>& a)
{
  // a is pre-allocated to size n — no resize needed
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

  ADDON_STATUS Create() override
  {
    std::string shaderStr = "1";
    kodi::addon::CheckSettingString("shader", shaderStr);
    m_shaderIdx = std::stoi(shaderStr);
    kodi::addon::CheckSettingBoolean("blur_enabled", m_blurEnabled);
    return ADDON_STATUS_OK;
  }

  ADDON_STATUS SetSetting(const std::string& settingName,
                          const kodi::addon::CSettingValue& settingValue) override
  {
    if (settingName == "shader")       m_shaderIdx   = std::stoi(settingValue.GetString());
    if (settingName == "blur_enabled") m_blurEnabled = settingValue.GetBoolean();
    return ADDON_STATUS_OK;
  }

  bool Start(int /*channels*/, int /*samplesPerSec*/, int /*bitsPerSample*/,
             const std::string& /*songName*/) override
  {
    if (!m_glReady) InitGL();
    m_currentArt.clear();
    m_pendingLoad = true;
    m_pendingText = true;
    m_startTime = std::chrono::steady_clock::now();
    m_prevFBO   = -1;  // re-capture on first render after track start
    return true;
  }

  void Stop() override {}

  void AudioData(const float* data, size_t length) override
  {
    int count = std::min((int)length / 2, kAudioW);
    for (int i = 0; i < count; i++)
      m_monoBuf[i] = (data[i * 2] + data[i * 2 + 1]) * 0.5f;
    for (int i = count; i < kAudioW; i++)
      m_monoBuf[i] = 0.f;

    for (int i = 0; i < kAudioW; i++)
      m_waveData[i] = m_monoBuf[i] * 0.5f + 0.5f;

    ComputeFFTMagnitudes(m_monoBuf.data(), kAudioW, m_freqData, m_fftBuf);
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
    {
      m_pendingText = false;
      RebuildLayout(vw, vh);   // also (re)creates FBOs on size change
    }
    // Only upload audio texture when the active shader actually reads it
    static const bool kUsesAudio[] = {true,false,false,true,false,false,true,true};
    bool needsAudio = (m_shaderIdx >= 0 && m_shaderIdx <= 7) && kUsesAudio[m_shaderIdx];
    if (m_audioTexDirty && needsAudio) { m_audioTexDirty = false; UploadAudioTex(); }

    float elapsed = std::fmod(
        std::chrono::duration<float>(
            std::chrono::steady_clock::now() - m_startTime).count(),
        3600.0f);   // wrap at 1 h — keeps sin/cos args small on old GPU

    // Cache Kodi's framebuffer binding (queried once per track, not every frame)
    if (m_prevFBO < 0)
      glGetIntegerv(GL_FRAMEBUFFER_BINDING, &m_prevFBO);
    GLint prevFBO = m_prevFBO;
    // Viewport is always (0,0,vw,vh) when Kodi calls Render() — use directly

    // ── Pass 1: render background into half-res FBO A ────────────────────────
    if (m_fboA)
    {
      glBindFramebuffer(GL_FRAMEBUFFER, m_fboA);
      glViewport(0, 0, m_fboW, m_fboH);
    }

    if (m_shaderIdx == 1)
    {
      glUseProgram(m_bgProgram1);
      glUniform2f(m_locResolution1, (float)m_fboW, (float)m_fboH);
      glUniform1f(m_locTime1, elapsed);
      glUniform3f(m_locHighlight1, m_highlightColor[0], m_highlightColor[1], m_highlightColor[2]);
      glUniform3f(m_locDark1,      m_darkColor[0],      m_darkColor[1],      m_darkColor[2]);
    }
    else if (m_shaderIdx == 2)
    {
      glUseProgram(m_bgProgram2);
      glUniform2f(m_locResolution2, (float)m_fboW, (float)m_fboH);
      glUniform1f(m_locTime2, elapsed);
    }
    else if (m_shaderIdx == 3)
    {
      glUseProgram(m_bgProgram3);
      glUniform2f(m_locResolution3, (float)m_fboW, (float)m_fboH);
      glUniform1f(m_locTime3, elapsed);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, m_audioTex);
      glUniform1i(m_locAudio3, 0);
    }
    else if (m_shaderIdx == 4)
    {
      glUseProgram(m_bgProgram4);
      glUniform2f(m_locResolution4, (float)m_fboW, (float)m_fboH);
      glUniform1f(m_locTime4, elapsed);
    }
    else if (m_shaderIdx == 5)
    {
      glClearColor(0.f, 0.f, 0.f, 1.f);
      glClear(GL_COLOR_BUFFER_BIT);
    }
    else if (m_shaderIdx == 6)
    {
      glUseProgram(m_bgProgram6);
      glUniform2f(m_locResolution6, (float)m_fboW, (float)m_fboH);
      glUniform1f(m_locTime6, elapsed);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, m_audioTex);
      glUniform1i(m_locAudio6, 0);
    }
    else if (m_shaderIdx == 7)
    {
      glUseProgram(m_bgProgram7);
      glUniform2f(m_locResolution7, (float)m_fboW, (float)m_fboH);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, m_audioTex);
      glUniform1i(m_locAudio7, 0);
    }
    else if (m_shaderIdx == 8)
    {
      glUseProgram(m_bgProgram8);
      glUniform2f(m_locResolution8, (float)m_fboW, (float)m_fboH);
      glUniform1f(m_locTime8, elapsed);
      glUniform3f(m_locHighlight8, m_highlightColor[0], m_highlightColor[1], m_highlightColor[2]);
      glUniform3f(m_locDark8,      m_darkColor[0],      m_darkColor[1],      m_darkColor[2]);
    }
    else
    {
      glUseProgram(m_bgProgram0);
      glUniform2f(m_locResolution0, (float)m_fboW, (float)m_fboH);
      glUniform1f(m_locTime0, elapsed);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, m_audioTex);
      glUniform1i(m_locAudio0, 0);
    }
    DrawFullscreen();

    // ── Pass 2: horizontal blur FBO A → FBO B ────────────────────────────────
    if (m_blurEnabled && m_fboA && m_fboB)
    {
      glBindFramebuffer(GL_FRAMEBUFFER, m_fboB);
      glUseProgram(m_blurProgram);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, m_fboTexA);
      glUniform1i(m_locBlurTex, 0);
      glUniform2f(m_locBlurDir, 1.f / (float)m_fboW, 0.f);
      DrawFullscreen();

      // ── Pass 3: vertical blur FBO B → screen ─────────────────────────────
      glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
      glViewport(0, 0, vw, vh);
      glBindTexture(GL_TEXTURE_2D, m_fboTexB);
      glUniform2f(m_locBlurDir, 0.f, 1.f / (float)m_fboH);
      DrawFullscreen();
    }
    else if (m_fboA)
    {
      // Blur disabled — blit FBO A to screen unblurred
      glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
      glViewport(0, 0, vw, vh);
      glUseProgram(m_program);
      glUniform1f(m_locAlpha, 1.0f);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, m_fboTexA);
      glUniform1i(m_locTex, 0);
      DrawFullscreen();
    }
    else
    {
      glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
      glViewport(0, 0, vw, vh);
    }

    // ── Art + text on top ─────────────────────────────────────────────────────
    glUseProgram(m_program);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (m_artTex && m_texW > 0)
    {
      // Solid outline — 4 px border on all sides using the art highlight colour
      float bx = 4.f * m_ndcPerPx, by = 4.f * m_ndcPerPxH;
      DrawSolidRect(m_artX0 - bx, m_artY0 - by, m_artX1 + bx, m_artY1 + by,
                    m_highlightColor[0], m_highlightColor[1], m_highlightColor[2], 0.80f);
      DrawQuad(m_artTex, m_artX0, m_artY0, m_artX1, m_artY1, 1.0f);
    }

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

    m_solidProgram = LinkProgram(VERT_SRC, SOLID_FRAG_SRC);
    if (!m_solidProgram) return false;
    m_locSolidColor = glGetUniformLocation(m_solidProgram, "u_color");

    m_blurProgram = LinkProgram(VERT_SRC, BLUR_FRAG_SRC);
    if (!m_blurProgram) return false;
    m_locBlurTex = glGetUniformLocation(m_blurProgram, "u_tex");
    m_locBlurDir = glGetUniformLocation(m_blurProgram, "u_dir");

    m_bgProgram0 = LinkProgram(VERT_SRC, BG0_FRAG_SRC);
    if (!m_bgProgram0) return false;
    m_locResolution0 = glGetUniformLocation(m_bgProgram0, "iResolution");
    m_locTime0       = glGetUniformLocation(m_bgProgram0, "iTime");
    m_locAudio0      = glGetUniformLocation(m_bgProgram0, "iChannel3");

    m_bgProgram1 = LinkProgram(VERT_SRC, BG1_FRAG_SRC);
    if (!m_bgProgram1) return false;
    m_locResolution1 = glGetUniformLocation(m_bgProgram1, "iResolution");
    m_locTime1       = glGetUniformLocation(m_bgProgram1, "iTime");
    m_locHighlight1  = glGetUniformLocation(m_bgProgram1, "iHighlightColor");
    m_locDark1       = glGetUniformLocation(m_bgProgram1, "iDarkColor");

    m_bgProgram2 = LinkProgram(VERT_SRC, BG2_FRAG_SRC);
    if (!m_bgProgram2) return false;
    m_locResolution2 = glGetUniformLocation(m_bgProgram2, "iResolution");
    m_locTime2       = glGetUniformLocation(m_bgProgram2, "iTime");

    m_bgProgram3 = LinkProgram(VERT_SRC, BG3_FRAG_SRC);
    if (!m_bgProgram3) return false;
    m_locResolution3 = glGetUniformLocation(m_bgProgram3, "iResolution");
    m_locTime3       = glGetUniformLocation(m_bgProgram3, "iTime");
    m_locAudio3      = glGetUniformLocation(m_bgProgram3, "iChannel0");

    m_bgProgram4 = LinkProgram(VERT_SRC, BG4_FRAG_SRC);
    if (!m_bgProgram4) return false;
    m_locResolution4 = glGetUniformLocation(m_bgProgram4, "iResolution");
    m_locTime4       = glGetUniformLocation(m_bgProgram4, "iTime");

    m_bgProgram6 = LinkProgram(VERT_SRC, BG6_FRAG_SRC);
    if (!m_bgProgram6) return false;
    m_locResolution6 = glGetUniformLocation(m_bgProgram6, "iResolution");
    m_locTime6       = glGetUniformLocation(m_bgProgram6, "iTime");
    m_locAudio6      = glGetUniformLocation(m_bgProgram6, "iChannel0");

    m_bgProgram7 = LinkProgram(VERT_SRC, BG7_FRAG_SRC);
    if (!m_bgProgram7) return false;
    m_locResolution7 = glGetUniformLocation(m_bgProgram7, "iResolution");
    m_locAudio7      = glGetUniformLocation(m_bgProgram7, "iChannel0");

    m_bgProgram8 = LinkProgram(VERT_SRC, BG8_FRAG_SRC);
    if (!m_bgProgram8) return false;
    m_locResolution8 = glGetUniformLocation(m_bgProgram8, "iResolution");
    m_locTime8       = glGetUniformLocation(m_bgProgram8, "iTime");
    m_locHighlight8  = glGetUniformLocation(m_bgProgram8, "iHighlightColor");
    m_locDark8       = glGetUniformLocation(m_bgProgram8, "iDarkColor");

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

  // ── Blur FBOs ───────────────────────────────────────────────────────────────

  void CreateFBOs(int vw, int vh)
  {
    DestroyFBOs();
    m_fboW = std::max(1, vw / 2);
    m_fboH = std::max(1, vh / 2);

    GLuint* fbos[2]  = { &m_fboA,    &m_fboB    };
    GLuint* texs[2]  = { &m_fboTexA, &m_fboTexB };
    for (int i = 0; i < 2; i++)
    {
      glGenTextures(1, texs[i]);
      glBindTexture(GL_TEXTURE_2D, *texs[i]);
      // Prefer 16-bit float for banding-free gradients; fall back to 8-bit if
      // the driver rejects the format (e.g. GLES 2.0 without float-buffer ext).
      glGetError();
#if defined(HAS_GLES)
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_fboW, m_fboH, 0, GL_RGBA, GL_HALF_FLOAT_OES, nullptr);
#else
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_fboW, m_fboH, 0, GL_RGBA, GL_FLOAT, nullptr);
#endif
      if (glGetError() != GL_NO_ERROR)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_fboW, m_fboH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindTexture(GL_TEXTURE_2D, 0);

      glGenFramebuffers(1, fbos[i]);
      glBindFramebuffer(GL_FRAMEBUFFER, *fbos[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *texs[i], 0);

      GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
      if (status != GL_FRAMEBUFFER_COMPLETE)
      {
        kodi::Log(ADDON_LOG_WARNING, "[AlbumArt] FBO incomplete: %d", (int)status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        DestroyFBOs();
        return;
      }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }

  void DestroyFBOs()
  {
    if (m_fboA)    { glDeleteFramebuffers(1, &m_fboA);    m_fboA = 0; }
    if (m_fboB)    { glDeleteFramebuffers(1, &m_fboB);    m_fboB = 0; }
    if (m_fboTexA) { glDeleteTextures(1, &m_fboTexA);     m_fboTexA = 0; }
    if (m_fboTexB) { glDeleteTextures(1, &m_fboTexB);     m_fboTexB = 0; }
    m_fboW = m_fboH = 0;
  }

  void UploadAudioTex()
  {
    for (int i = 0; i < kAudioW; i++)
    {
      uint8_t fv = (uint8_t)(std::min(1.f, m_freqData[i]) * 255.f);
      uint8_t wv = (uint8_t)(std::max(0.f, std::min(1.f, m_waveData[i])) * 255.f);
      int f4 = i * 4, w4 = (kAudioW + i) * 4;
      m_audioPixels[f4]=m_audioPixels[f4+1]=m_audioPixels[f4+2]=fv; m_audioPixels[f4+3]=255;
      m_audioPixels[w4]=m_audioPixels[w4+1]=m_audioPixels[w4+2]=wv; m_audioPixels[w4+3]=255;
    }
    glBindTexture(GL_TEXTURE_2D, m_audioTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kAudioW, 2, GL_RGBA, GL_UNSIGNED_BYTE, m_audioPixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  void DeinitGL()
  {
    DestroyFBOs();
    DeleteArtTexture();
    m_texTitle.destroy(); m_texArtist.destroy(); m_texAlbum.destroy();
    if (m_audioTex)    { glDeleteTextures(1, &m_audioTex);    m_audioTex = 0; }
    if (m_vbo)         { glDeleteBuffers(1, &m_vbo);           m_vbo = 0; }
#if !defined(HAS_GLES)
    if (m_vao)         { glDeleteVertexArrays(1, &m_vao);      m_vao = 0; }
#endif
    if (m_bgProgram8)  { glDeleteProgram(m_bgProgram8);        m_bgProgram8 = 0; }
    if (m_bgProgram7)  { glDeleteProgram(m_bgProgram7);        m_bgProgram7 = 0; }
    if (m_bgProgram6)  { glDeleteProgram(m_bgProgram6);        m_bgProgram6 = 0; }
    if (m_bgProgram4)  { glDeleteProgram(m_bgProgram4);        m_bgProgram4 = 0; }
    if (m_bgProgram3)  { glDeleteProgram(m_bgProgram3);        m_bgProgram3 = 0; }
    if (m_bgProgram2)  { glDeleteProgram(m_bgProgram2);        m_bgProgram2 = 0; }
    if (m_bgProgram1)  { glDeleteProgram(m_bgProgram1);        m_bgProgram1 = 0; }
    if (m_bgProgram0)  { glDeleteProgram(m_bgProgram0);        m_bgProgram0 = 0; }
    if (m_blurProgram)  { glDeleteProgram(m_blurProgram);       m_blurProgram = 0; }
    if (m_solidProgram) { glDeleteProgram(m_solidProgram);      m_solidProgram = 0; }
    if (m_program)      { glDeleteProgram(m_program);           m_program = 0; }
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
    glBindVertexArray(m_vao); glDrawArrays(GL_TRIANGLE_FAN, 0, 4); glBindVertexArray(0);
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

  void DrawSolidRect(float x0, float y0, float x1, float y1, float r, float g, float b, float a)
  {
    float verts[16] = {
      x0,y0, 0.f,0.f,  x1,y0, 1.f,0.f,
      x1,y1, 1.f,1.f,  x0,y1, 0.f,1.f,
    };
    glUseProgram(m_solidProgram);
    glUniform4f(m_locSolidColor, r, g, b, a);
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
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(m_program);  // restore overlay program
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
    ComputeArtPalette(data, w, h);
    stbi_image_free(data);
    m_texW = w; m_texH = h; m_viewW = 0;
    return true;
  }

  // Sort up to 2048 sampled pixels by luminance; average bottom 15% → m_darkColor,
  // top 15% → m_highlightColor. Used for metaballs coloring and art outline.
  void ComputeArtPalette(const unsigned char* data, int w, int h)
  {
    int total = w * h;
    // Sample up to 10 000 pixels (every pixel for small images)
    int step = std::max(1, total / 10000);
    struct Pix { float r, g, b, lum; };
    std::vector<Pix> pix;
    pix.reserve(std::min(total, 10000));
    for (int i = 0; i < total; i += step) {
      float r = data[i*4+0] / 255.f, g = data[i*4+1] / 255.f, b = data[i*4+2] / 255.f;
      pix.push_back({r, g, b, 0.299f*r + 0.587f*g + 0.114f*b});
    }
    // Sort by luminance: bottom 15% = background (darkest), top 15% = foreground (lightest)
    std::sort(pix.begin(), pix.end(), [](const Pix& a, const Pix& b){ return a.lum < b.lum; });
    int n = (int)pix.size();
    int lo = std::max(1, n * 15 / 100);
    double dR=0,dG=0,dB=0, hR=0,hG=0,hB=0;
    for (int j = 0;    j < lo;     j++) { dR+=pix[j].r; dG+=pix[j].g; dB+=pix[j].b; }
    for (int j = n-lo; j < n;      j++) { hR+=pix[j].r; hG+=pix[j].g; hB+=pix[j].b; }
    m_darkColor[0]      = (float)(dR/lo); m_darkColor[1]      = (float)(dG/lo); m_darkColor[2]      = (float)(dB/lo);
    m_highlightColor[0] = (float)(hR/lo); m_highlightColor[1] = (float)(hG/lo); m_highlightColor[2] = (float)(hB/lo);
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
    std::vector<std::string> med = {
      kodi::addon::GetAddonPath("fonts/Roboto-Medium.ttf"),
    };
#if defined(TARGET_DARWIN)
    reg.push_back("/Library/Fonts/Arial.ttf");
    reg.push_back("/System/Library/Fonts/Supplemental/Arial.ttf");
#elif defined(TARGET_ANDROID)
    reg.push_back("/system/fonts/Roboto-Regular.ttf");
    ital.push_back("/system/fonts/Roboto-Italic.ttf");
    med.push_back("/system/fonts/Roboto-Medium.ttf");
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
    LoadFontFromCandidates(med,  m_fontDataMedium,  m_fontInfoMedium);
    return true;
  }

  // ── Text rasterization ──────────────────────────────────────────────────────

  // Decode one UTF-8 codepoint from s at position i, advance i.
  static int Utf8Next(const std::string& s, size_t& i)
  {
    unsigned char c0 = (unsigned char)s[i];
    if (c0 < 0x80) { i++; return c0; }
    int len, cp;
    if      ((c0 & 0xE0) == 0xC0) { len = 2; cp = c0 & 0x1F; }
    else if ((c0 & 0xF0) == 0xE0) { len = 3; cp = c0 & 0x0F; }
    else if ((c0 & 0xF8) == 0xF0) { len = 4; cp = c0 & 0x07; }
    else { i++; return 0xFFFD; }
    for (int j = 1; j < len; j++) {
      if (i + j >= s.size() || ((unsigned char)s[i+j] & 0xC0) != 0x80) { i++; return 0xFFFD; }
      cp = (cp << 6) | ((unsigned char)s[i+j] & 0x3F);
    }
    i += len;
    return cp;
  }

  TextTex MakeTextTex(const std::string& text, float pixelH, bool italic, bool bold = false)
  {
    TextTex out;
    if (text.empty() || m_fontData.empty()) return out;
    bool useShear      = italic && m_fontDataItalic.empty();
    stbtt_fontinfo& fi = bold   && !m_fontDataMedium.empty()   ? m_fontInfoMedium  :
                         italic && !m_fontDataItalic.empty()   ? m_fontInfoItalic  :
                                                                  m_fontInfo;
    float scale = stbtt_ScaleForPixelHeight(&fi, pixelH);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&fi, &ascent, &descent, &lineGap);
    int asc   = (int)(ascent  * scale + 0.5f);
    int dsc   = (int)(descent * scale - 0.5f);
    int lineH = asc - dsc;
    int totalW = 0;
    for (size_t i = 0; i < text.size(); )
    {
      int cp = Utf8Next(text, i);
      if (cp < 32) continue;
      int adv, lsb; stbtt_GetCodepointHMetrics(&fi, cp, &adv, &lsb);
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
    for (size_t i = 0; i < text.size(); )
    {
      int cp = Utf8Next(text, i);
      if (cp < 32) continue;
      int adv, lsb; stbtt_GetCodepointHMetrics(&fi, cp, &adv, &lsb);
      int x0g, y0g, x1g, y1g;
      stbtt_GetCodepointBitmapBox(&fi, cp, scale, scale, &x0g, &y0g, &x1g, &y1g);
      int gw = x1g - x0g, gh = y1g - y0g;
      if (gw > 0 && gh > 0)
      {
        std::vector<uint8_t> glyph(gw * gh);
        stbtt_MakeCodepointBitmap(&fi, glyph.data(), gw, gh, gw, scale, scale, cp);
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
    if (vw != m_viewW || vh != m_viewH)
      CreateFBOs(vw, vh);

    m_viewW = vw; m_viewH = vh;
    m_ndcPerPx  = 2.f / (float)vw;
    m_ndcPerPxH = 2.f / (float)vh;

    // ── Album art ──────────────────────────────────────────────────────────
    // Left margin 6.1 % of canvas width; art height 42.5 % of canvas height;
    // art centred vertically; aspect-ratio preserved (max 55 % of canvas width).
    float mxNdc   = 2.f * 0.061f;
    float artHNdc = 2.f * 0.425f;
    float artAR   = (m_texW > 0 && m_texH > 0) ? (float)m_texW / (float)m_texH : 1.f;
    float artWNdc = std::min(artHNdc * artAR * ((float)vh / (float)vw), 0.55f);

    m_artX0 = -1.f + mxNdc;
    m_artX1 =  m_artX0 + artWNdc;
    m_artY0 = -artHNdc * 0.5f;
    m_artY1 =  artHNdc * 0.5f;

    // ── Text panel ─────────────────────────────────────────────────────────
    // 6.1 % of canvas width gap between art right edge and text; right margin
    // matches left margin.
    float textX0   = m_artX1 + 2.f * 0.061f;
    float textX1   = 1.f - mxNdc * 0.8f;
    float textWNdc = textX1 - textX0;

    // Font sizes from layout.svg (as fraction of canvas height):
    //   title 5.93 %, artist 3.46 %, album 2.96 %
    float szTitle  = std::max(16.f, std::min(70.f, (float)vh * 0.0593f));
    float szArtist = std::max(10.f, std::min(42.f, (float)vh * 0.0346f));
    float szAlbum  = std::max( 8.f, std::min(36.f, (float)vh * 0.0296f));

    LoadFont();
    m_texTitle.destroy(); m_texArtist.destroy(); m_texAlbum.destroy();
    m_texTitle  = MakeTextTex(m_title.empty()  ? " " : m_title,  szTitle,  false, true);
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

    // Baseline-to-baseline gaps from layout.svg:
    //   title→artist  0.920 × title line height
    //   artist→album  1.091 × artist line height
    // Convert to visual gap (bottom-of-A to top-of-B) assuming descender ≈ 20 %
    // of line height:  gap = (kB − kDsc) × hA − (1 − kDsc) × hB
    const float kDsc = 0.20f;
    float gapTA = (0.920f - kDsc) * hTitle  - (1.f - kDsc) * hArtist;
    float gapAA = (1.091f - kDsc) * hArtist - (1.f - kDsc) * hAlbum;
    gapTA = std::max(0.f, gapTA);
    gapAA = std::max(0.f, gapAA);

    // Centre the text block against the art (both centred at NDC y = 0).
    float blockH   = hTitle + gapTA + hArtist + gapAA + hAlbum;
    float blockTop = blockH * 0.5f;
    m_titleY  = blockTop  - hTitle;
    m_artistY = m_titleY  - gapTA - hArtist;
    m_albumY  = m_artistY - gapAA - hAlbum;

    m_titleX  = textX0 - m_texTitle.xOff  * m_ndcPerPx;
    m_artistX = textX0 - m_texArtist.xOff * m_ndcPerPx;
    m_albumX  = textX0 - m_texAlbum.xOff  * m_ndcPerPx;
  }

  // ── State ───────────────────────────────────────────────────────────────────

  bool   m_glReady       = false;
  bool   m_pendingLoad   = false;
  bool   m_pendingText   = false;
  bool   m_audioTexDirty = false;
  int    m_shaderIdx     = 1;
  bool   m_blurEnabled   = true;

  GLuint m_program      = 0;
  GLint  m_locTex       = -1;
  GLint  m_locAlpha     = -1;

  GLuint m_solidProgram  = 0;
  GLint  m_locSolidColor = -1;

  GLuint m_blurProgram = 0;
  GLint  m_locBlurTex  = -1;
  GLint  m_locBlurDir  = -1;

  GLuint m_bgProgram0     = 0;
  GLint  m_locResolution0 = -1;
  GLint  m_locTime0       = -1;
  GLint  m_locAudio0      = -1;

  GLuint m_bgProgram1     = 0;
  GLint  m_locResolution1 = -1;
  GLint  m_locTime1       = -1;

  GLuint m_bgProgram2     = 0;
  GLint  m_locResolution2 = -1;
  GLint  m_locTime2       = -1;

  GLuint m_bgProgram3     = 0;
  GLint  m_locResolution3 = -1;
  GLint  m_locTime3       = -1;
  GLint  m_locAudio3      = -1;

  GLuint m_bgProgram4     = 0;
  GLint  m_locResolution4 = -1;
  GLint  m_locTime4       = -1;

  GLuint m_bgProgram6     = 0;
  GLint  m_locResolution6 = -1;
  GLint  m_locTime6       = -1;
  GLint  m_locAudio6      = -1;

  GLuint m_bgProgram7     = 0;
  GLint  m_locResolution7 = -1;
  GLint  m_locAudio7      = -1;

  GLuint m_bgProgram8     = 0;
  GLint  m_locResolution8 = -1;
  GLint  m_locTime8       = -1;
  GLint  m_locHighlight1  = -1;
  GLint  m_locDark1       = -1;
  GLint  m_locHighlight8  = -1;
  GLint  m_locDark8       = -1;
  float  m_highlightColor[3] = {0.85f, 0.85f, 0.90f};  // default; updated per album
  float  m_darkColor[3]      = {0.04f, 0.04f, 0.08f};

  // Blur FBOs (half-res: bg→fboA, H-blur→fboB, V-blur→screen)
  GLuint m_fboA = 0, m_fboTexA = 0;
  GLuint m_fboB = 0, m_fboTexB = 0;
  int    m_fboW = 0, m_fboH = 0;

  GLuint m_vao = 0;
  GLuint m_vbo = 0;

  GLuint m_audioTex = 0;
  float  m_freqData[kAudioW] = {};
  float  m_waveData[kAudioW] = {};
  std::vector<float>               m_monoBuf     = std::vector<float>(kAudioW, 0.f);
  std::vector<std::complex<float>> m_fftBuf      = std::vector<std::complex<float>>(kAudioW);
  std::vector<uint8_t>             m_audioPixels = std::vector<uint8_t>(kAudioW * 2 * 4, 128);

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
  std::vector<uint8_t> m_fontDataMedium;
  stbtt_fontinfo       m_fontInfoMedium = {};

  std::chrono::steady_clock::time_point m_startTime;
  GLint m_prevFBO = -1;
};

ADDONCREATOR(CVisualizationAlbumArt)
