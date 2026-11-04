/* paltest -- does MiniGL's GL_MGL_palette_lookup (the R200's two-pass
 * fragment shader with a dependent read) land every index on the right
 * palette texel?
 *
 * Unit 0: an index texture, red = the index (0..255 across x).
 * Unit 1: a 512 x 512 palette texture whose texels name themselves:
 *   test A: R = (x & 31) << 3, G = (x >> 5) << 2, B = (y & 31) << 3
 *   test B: R = (y & 31) << 3, G = (y >> 5) << 2   (the row)
 * (exact through a 16-bit RGB565 frame buffer). A strip of 256 pixels,
 * one per index, is drawn and read back with glReadPixels; test B draws
 * 256 one-pixel rows, one per vertex alpha (the palette row).
 * Writes the maps to stdout. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/minigl.h>

#ifndef MGL_PALETTE_LOOKUP
#define MGL_PALETTE_LOOKUP 0x7021
#endif
#ifndef MGL_UBYTE_BGRA
#define MGL_UBYTE_BGRA 0x7018
#endif

#define W 640
#define H 480

static unsigned char *s_pal, *s_idx, *s_rb;

static void env_default(const char *name, const char *value)
{
    char buf[8];
    if (GetVar((CONST_STRPTR)name, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) < 0)
        SetVar((CONST_STRPTR)name, (CONST_STRPTR)value, -1, GVF_GLOBAL_ONLY);
}

static GLuint make_tex(int w, int h, const unsigned char *bgra)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, bgra);
    return t;
}

static void fill_palette(int rowtest)
{
    int x, y;
    for (y = 0; y < 512; y++)
        for (x = 0; x < 512; x++) {
            unsigned char *p = s_pal + ((size_t)y * 512 + x) * 4;
            int v = rowtest ? y : x;
            p[0] = rowtest ? 0 : (unsigned char)((y & 31) << 3);   /* B */
            p[1] = (unsigned char)((v >> 5) << 2);                  /* G */
            p[2] = (unsigned char)((v & 31) << 3);                  /* R */
            p[3] = 255;
        }
}

/* the frame read back; returns the row (from the top) whose pixel x0 is not
 * black, scanning rows y0..y1 in both orientations */
static const unsigned char *px(int x, int y_from_top)
{
    return s_rb + ((size_t)(H - 1 - y_from_top) * W + x) * 4;   /* GL: bottom row first */
}

static int decode(const unsigned char *p) { return (p[0] >> 3) | ((p[1] >> 2) << 5); }

int main(int argc, char **argv)
{
    GLuint tidx, tpal;
    int i, x, y, errors = 0, lastv = -1;
    int alpha = argc > 1 ? atoi(argv[1]) : 64;

    env_default("MGLPPC_NOALTIVEC", "1");
    env_default("MGLPPC_CPEMIT", "1");
    env_default("MGLPPC_HOSTPRI", "-1");
    env_default("MGLPPC_CPPRESENT", "1");
    env_default("MGLPPC_TCL6", "1");
    env_default("MGLPPC_TCL5", "1");
    s_pal = malloc(512 * 512 * 4);
    s_idx = malloc(256 * 2 * 4);
    s_rb = malloc(W * H * 4);
    if (!s_pal || !s_idx || !s_rb)
        return 20;
    if (!MiniGLOpen()) {
        printf("MiniGLOpen failed\n");
        return 20;
    }
    mglChooseWindowMode(GL_FALSE);
    mglChoosePixelDepth(16);
    mglChooseNumberOfBuffers(2);
    if (!mglCreateContext(0, 0, W, H)) {
        printf("mglCreateContext failed\n");
        MiniGLClose();
        return 20;
    }
    printf("extensions: %s\n", (const char *)glGetString(GL_EXTENSIONS));
    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, W, H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glClearColor(0, 0, 0, 1);

    for (i = 0; i < 512; i++) {          /* the index texture: 256 x 2, red = x */
        unsigned char *p = s_idx + (size_t)i * 4;
        p[0] = 0; p[1] = 0; p[2] = (unsigned char)(i & 255); p[3] = 255;
    }
    tidx = make_tex(256, 2, s_idx);
    fill_palette(0);
    tpal = make_tex(512, 512, s_pal);

    /* ---- test A: the index -> palette column ---- */
    glClear(GL_COLOR_BUFFER_BIT);
    glActiveTextureARB(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tpal);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glActiveTextureARB(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tidx);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(MGL_PALETTE_LOOKUP);
    printf("palette lookup enabled: %d, error %x\n", (int)glIsEnabled(MGL_PALETTE_LOOKUP), (unsigned)glGetError());
    glBegin(GL_QUADS);
    glColor4ub(255, 255, 255, (GLubyte)alpha);
    glTexCoord2f(0.0f, 0.25f); glVertex2f(100.0f, 100.0f);
    glTexCoord2f(1.0f, 0.25f); glVertex2f(356.0f, 100.0f);
    glTexCoord2f(1.0f, 0.25f); glVertex2f(356.0f, 104.0f);
    glTexCoord2f(0.0f, 0.25f); glVertex2f(100.0f, 104.0f);
    glEnd();
    glFinish();
    memset(s_rb, 0, W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, s_rb);
    printf("readback error %x; pixel(101,102) %02x%02x%02x%02x, flipped %02x%02x%02x%02x\n",
           (unsigned)glGetError(), px(101, 102)[0], px(101, 102)[1], px(101, 102)[2], px(101, 102)[3],
           s_rb[((size_t)102 * W + 101) * 4], s_rb[((size_t)102 * W + 101) * 4 + 1],
           s_rb[((size_t)102 * W + 101) * 4 + 2], s_rb[((size_t)102 * W + 101) * 4 + 3]);
    {
        int flipped = !(px(300, 102)[0] | px(300, 102)[1] | px(300, 102)[2]);
        printf("test A (alpha %d): index -> palette texel x (row bits)%s\n", alpha,
               flipped ? " [rows read top-first]" : "");
        for (x = 0; x < 256; x++) {
            const unsigned char *p = flipped ? s_rb + ((size_t)102 * W + 100 + x) * 4 : px(100 + x, 102);
            int v = decode(p);
            printf("%3d:%3d/%2d%s", x, v, p[2] >> 3, (x & 7) == 7 ? "\n" : " ");
            if (v <= lastv) errors++;
            lastv = v;
        }
        printf("test A: %d non-increasing\n", errors);
    }

    /* ---- test B: the vertex alpha -> palette row ---- */
    fill_palette(1);
    glBindTexture(GL_TEXTURE_2D, tpal);
    glActiveTextureARB(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tpal);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 512, 0, MGL_UBYTE_BGRA, GL_UNSIGNED_BYTE, s_pal);
    glActiveTextureARB(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tidx);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    for (y = 0; y < 256; y++) {
        glColor4ub(255, 255, 255, (GLubyte)y);
        glTexCoord2f(0.01f, 0.25f); glVertex2f(100.0f, 200.0f + y);
        glTexCoord2f(0.01f, 0.25f); glVertex2f(110.0f, 200.0f + y);
        glTexCoord2f(0.01f, 0.25f); glVertex2f(110.0f, 201.0f + y);
        glTexCoord2f(0.01f, 0.25f); glVertex2f(100.0f, 201.0f + y);
    }
    glEnd();
    glFinish();
    memset(s_rb, 0, W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, s_rb);
    {
        int flipped = !(px(105, 300)[0] | px(105, 300)[1] | px(105, 300)[2]);
        errors = 0; lastv = -1;
        printf("test B: vertex alpha -> palette row%s\n", flipped ? " [rows read top-first]" : "");
        for (y = 0; y < 256; y++) {
            const unsigned char *p = flipped ? s_rb + ((size_t)(200 + y) * W + 105) * 4 : px(105, 200 + y);
            int v = decode(p);
            printf("%3d:%3d%s", y, v, (y & 7) == 7 ? "\n" : " ");
            if (v <= lastv) errors++;
            lastv = v;
        }
        printf("test B: %d non-increasing\n", errors);
    }
    glDisable(MGL_PALETTE_LOOKUP);
    mglSwitchDisplay();
    mglDeleteContext();
    MiniGLClose();
    return 0;
}
