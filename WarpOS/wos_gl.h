/*
 * wos_gl.h -- GlesGpu's OpenGL on the WarpOS build.
 *
 * On the Wii GlesGpu draws through a fork of opengx whose GL is PSX-shaped:
 * glTexImage2D takes PSX texels and splits them into an opaque and a
 * semi-transparent plane, the glPRIMdraw* calls draw one PSX primitive in up
 * to two passes with the PSX blend modes, and so on. WarpOS/wos_gl.c gives
 * the same calls the same meaning over MiniGL (R200).
 *
 * This header is all GlesGpu sees: OpenGL's types and token values (MiniGL 29
 * uses OpenGL's own values), and every call it makes, renamed to wosgl_* so
 * that MiniGL's inline gl* functions -- which wos_gl.c uses -- never meet it.
 */
#ifndef WOS_GL_H
#define WOS_GL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef void           GLvoid;
typedef signed char    GLbyte;
typedef short          GLshort;
typedef int            GLint;
typedef unsigned char  GLubyte;
typedef unsigned short GLushort;
typedef unsigned int   GLuint;
typedef int            GLsizei;
typedef float          GLfloat;
typedef float          GLclampf;
typedef double         GLdouble;
typedef double         GLclampd;

#define GL_FALSE                  0
#define GL_TRUE                   1
#define GL_NONE                   0
#define GL_ZERO                   0
#define GL_ONE                    1
#define GL_NO_ERROR               0

#define GL_DEPTH_BUFFER_BIT       0x00000100
#define GL_COLOR_BUFFER_BIT       0x00004000

#define GL_NEVER                  0x0200
#define GL_LESS                   0x0201
#define GL_EQUAL                  0x0202
#define GL_LEQUAL                 0x0203
#define GL_GREATER                0x0204
#define GL_NOTEQUAL               0x0205
#define GL_GEQUAL                 0x0206
#define GL_ALWAYS                 0x0207

#define GL_SRC_COLOR              0x0300
#define GL_ONE_MINUS_SRC_COLOR    0x0301
#define GL_SRC_ALPHA              0x0302
#define GL_ONE_MINUS_SRC_ALPHA    0x0303
#define GL_DST_ALPHA              0x0304
#define GL_ONE_MINUS_DST_ALPHA    0x0305
#define GL_DST_COLOR              0x0306
#define GL_ONE_MINUS_DST_COLOR    0x0307

#define GL_CULL_FACE              0x0B44
#define GL_LIGHTING               0x0B50
#define GL_FOG                    0x0B60
#define GL_DEPTH_TEST             0x0B71
#define GL_STENCIL_TEST           0x0B90
#define GL_ALPHA_TEST             0x0BC0
#define GL_DITHER                 0x0BD0
#define GL_BLEND                  0x0BE2
#define GL_SCISSOR_TEST           0x0C11
#define GL_POINT_SMOOTH           0x0B10
#define GL_LINE_SMOOTH            0x0B20
#define GL_TEXTURE_2D             0x0DE1

#define GL_UNSIGNED_BYTE          0x1401
#define GL_UNSIGNED_SHORT         0x1403
#define GL_FLOAT                  0x1406
#define GL_TEXTURE                0x1702
#define GL_MODELVIEW              0x1700
#define GL_PROJECTION             0x1701
#define GL_RGB                    0x1907
#define GL_RGBA                   0x1908
#define GL_FLAT                   0x1D00
#define GL_SMOOTH                 0x1D01
#define GL_RENDERER               0x1F01
#define GL_VENDOR                 0x1F00
#define GL_VERSION                0x1F02
#define GL_EXTENSIONS             0x1F03

#define GL_MODULATE               0x2100
#define GL_TEXTURE_ENV_MODE       0x2200
#define GL_TEXTURE_ENV            0x2300
#define GL_NEAREST                0x2600
#define GL_LINEAR                 0x2601
#define GL_TEXTURE_MAG_FILTER     0x2800
#define GL_TEXTURE_MIN_FILTER     0x2801
#define GL_TEXTURE_WRAP_S         0x2802
#define GL_TEXTURE_WRAP_T         0x2803
#define GL_CLAMP                  0x2900
#define GL_REPEAT                 0x2901
#define GL_CLAMP_TO_EDGE          0x812F
#define GL_MIRRORED_REPEAT        0x8370
#define GL_BGRA_EXT               0x80E1
#define GL_COLOR_TABLE            0x80D0
#define GL_FUNC_ADD               0x8006
#define GL_FUNC_REVERSE_SUBTRACT  0x800B

/* EGL leftovers GlesGpu still names in dead code */
#define GL_SURFACE_TYPE           0x3033
#define GL_WINDOW_BIT             0x0004

/* ---- Standard calls, with opengx's meaning ---------------------------- */
#define glEnable            wosgl_Enable
#define glDisable           wosgl_Disable
#define glScissor           wosgl_Scissor
#define glViewport          wosgl_Viewport
#define glClear             wosgl_Clear
#define glGenTextures       wosgl_GenTextures
#define glDeleteTextures    wosgl_DeleteTextures
#define glTexParameteri     wosgl_TexParameteri
#define glTexParameterf     wosgl_TexParameterf
#define glTexEnvf           wosgl_TexEnvf
#define glTexImage2D        wosgl_TexImage2D
#define glTexSubImage2D     wosgl_TexSubImage2D
#define glCopyTexSubImage2D wosgl_CopyTexSubImage2D
#define glAlphaFunc         wosgl_AlphaFunc
#define glBlendFunc         wosgl_BlendFunc
#define glBlendEquationEXT  wosgl_BlendEquationEXT
#define glColorTableEXT     wosgl_ColorTableEXT
#define glDepthFunc         wosgl_DepthFunc
#define glMatrixMode        wosgl_MatrixMode
#define glLoadIdentity      wosgl_LoadIdentity
#define glOrtho             wosgl_Ortho
#define glScalef            wosgl_Scalef
#define glShadeModel        wosgl_ShadeModel
#define glGetError          wosgl_GetError
#define glGetString         wosgl_GetString
#define glFlush             wosgl_Flush
#define glFinish            wosgl_Finish

void  wosgl_Enable(GLenum cap);
void  wosgl_Disable(GLenum cap);
void  wosgl_Scissor(GLint x, GLint y, GLsizei width, GLsizei height);
void  wosgl_Viewport(GLint x, GLint y, GLsizei width, GLsizei height);
void  wosgl_Clear(GLbitfield mask);
void  wosgl_GenTextures(GLsizei n, GLuint *textures);
void  wosgl_DeleteTextures(GLsizei n, const GLuint *textures);
void  wosgl_TexParameteri(GLenum target, GLenum pname, GLint param);
void  wosgl_TexParameterf(GLenum target, GLenum pname, GLfloat param);
void  wosgl_TexEnvf(GLenum target, GLenum pname, GLfloat param);
/* PSX texels: 4 bytes each, the PSX BGR555 + STP halfword in bytes 2-3.
 * Returns which planes received texels (1: semi-transparent, 2: opaque). */
int   wosgl_TexImage2D(GLenum target, GLint level, GLint internalFormat,
                       GLsizei width, GLsizei height, GLint border,
                       GLenum format, GLenum type, const GLvoid *data);
int   wosgl_TexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                          GLsizei width, GLsizei height, GLenum format, GLenum type,
                          const GLvoid *data);
void  wosgl_CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                              GLint x, GLint y, GLsizei width, GLsizei height);
void  wosgl_AlphaFunc(GLenum func, GLclampf ref);
void  wosgl_BlendFunc(GLenum sfactor, GLenum dfactor);
void  wosgl_BlendEquationEXT(GLenum mode);
void  wosgl_ColorTableEXT(GLenum target, GLenum internalFormat, GLsizei width,
                          GLenum format, GLenum type, const GLvoid *data);
void  wosgl_DepthFunc(GLenum func);
void  wosgl_MatrixMode(GLenum mode);
void  wosgl_LoadIdentity(void);
void  wosgl_Ortho(int left, int right, int bottom, int top, int near_val, int far_val);
void  wosgl_Scalef(GLfloat x, GLfloat y, GLfloat z);
void  wosgl_ShadeModel(GLenum mode);
GLenum wosgl_GetError(void);
const GLubyte *wosgl_GetString(GLenum name);
void  wosgl_Flush(void);
void  wosgl_Finish(void);

/* ---- opengx's PSX extensions ------------------------------------------ */
#define glBindTextureBef                     wosgl_BindTextureBef
#define glNoNeedMulConstColor                wosgl_NoNeedMulConstColor
#define glClearColor2                        wosgl_ClearColor2
#define glColor4Lcol                         wosgl_Color4Lcol
#define glSetRGB24                           wosgl_SetRGB24
#define glSetTextureMask                     wosgl_SetTextureMask
#define glSetGlobalTextABR                   wosgl_SetGlobalTextABR
#define glSetTextureType                     wosgl_SetTextureType
#define glSetLoadMtxFlg                      wosgl_SetLoadMtxFlg
#define glSetVramClearedFlg                  wosgl_SetVramClearedFlg
#define glInitRGBATextures                   wosgl_InitRGBATextures
#define glInitMovieTextures                  wosgl_InitMovieTextures
#define glResetMovieTexPtr                   wosgl_ResetMovieTexPtr
#define glCaptureFramebufferTexture          wosgl_CaptureFramebufferTexture
#define glCaptureFramebufferIntensityTexture wosgl_CaptureFramebufferIntensityTexture
#define glPRIMdrawTexturedQuad               wosgl_PRIMdrawTexturedQuad
#define glPRIMdrawTexturedTri                wosgl_PRIMdrawTexturedTri
#define glPRIMdrawTexGouraudTriColor         wosgl_PRIMdrawTexGouraudTriColor
#define glPRIMdrawTexGouraudTriColorQuad     wosgl_PRIMdrawTexGouraudTriColorQuad
#define glPRIMdrawTri                        wosgl_PRIMdrawTri
#define glPRIMdrawTri2                       wosgl_PRIMdrawTri2
#define glPRIMdrawGouraudTriColor            wosgl_PRIMdrawGouraudTriColor
#define glPRIMdrawGouraudTri2Color           wosgl_PRIMdrawGouraudTri2Color
#define glPRIMdrawFlatLine                   wosgl_PRIMdrawFlatLine
#define glPRIMdrawGouraudLine                wosgl_PRIMdrawGouraudLine
#define glPRIMdrawQuad                       wosgl_PRIMdrawQuad

void  wosgl_BindTextureBef(GLenum target, GLuint texture);
void  wosgl_NoNeedMulConstColor(short flag);
void  wosgl_ClearColor2(unsigned char red, unsigned char green, unsigned char blue, unsigned char alpha);
void  wosgl_Color4Lcol(unsigned int lcol);
void  wosgl_SetRGB24(short rgb24);
void  wosgl_SetTextureMask(short mask);
void  wosgl_SetGlobalTextABR(short abr);
void  wosgl_SetTextureType(short textureSemiType, short loadTextureType, short textureChgType);
void  wosgl_SetLoadMtxFlg(void);
void  wosgl_SetVramClearedFlg(void);
void  wosgl_InitRGBATextures(GLsizei width, GLsizei height);
int   wosgl_InitMovieTextures(GLsizei width, GLsizei height, void *texData);
void  wosgl_ResetMovieTexPtr(void);
int   wosgl_CaptureFramebufferTexture(GLsizei srcWidth, GLsizei srcHeight);
int   wosgl_CaptureFramebufferIntensityTexture(GLsizei srcWidth, GLsizei srcHeight);
/* One PSX primitive: vertices of 24 bytes (float x, y, z, s, t; the colour
 * word, R G B A in its bytes 0-3). */
void  wosgl_PRIMdrawTexturedQuad(void *vertexAdr, int changePointOrder);
void  wosgl_PRIMdrawTexturedTri(void *vertexAdr);
void  wosgl_PRIMdrawTexGouraudTriColor(void *vertexAdr);
void  wosgl_PRIMdrawTexGouraudTriColorQuad(void *vertexAdr);
void  wosgl_PRIMdrawTri(void *vertexAdr);
void  wosgl_PRIMdrawTri2(void *vertexAdr);
void  wosgl_PRIMdrawGouraudTriColor(void *vertexAdr);
void  wosgl_PRIMdrawGouraudTri2Color(void *vertexAdr);
void  wosgl_PRIMdrawFlatLine(void *vertexAdr);
void  wosgl_PRIMdrawGouraudLine(void *vertexAdr);
void  wosgl_PRIMdrawQuad(void *vertexAdr);

/* GlesGpu's VRAM readback tracks every draw it submits (opengx's
 * ogx_draw_submitted_cb); called once per primitive pass. */
extern void (*ogx_draw_submitted_cb)(void);

/* opengx's start-up: here, reset the layer's state (the MiniGL context
 * itself is WarpOS/wos_video.c's). */
void  ogx_initialize(void);

#ifdef __cplusplus
}
#endif

#endif
