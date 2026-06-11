// doomgeneric PS2 backend -- ps2gl (hardware VU1+GS) video, MILESTONE 1.
//
// This is the foundation step for a GL world renderer. For now it does NOT
// render the world in GL yet: it brings ps2gl up inside the real game build
// (alongside libSDL2main's entry, audsrv, the game loop and the boot console)
// expands Doom's 8-bit (CMAP256) framebuffer through the palette into an RGBA
// texture and draws it on a fullscreen quad. That proves the integration --
// C++ linking, ps2gl init
// without the glut main loop, and coexistence with our audio -- before the
// real renderer (walls/flats/sprites as GL geometry) is built on top.
//
// We reuse glutInit() only for the GS / ps2gl bring-up (GIF reset, SetGsCrt,
// pglInit, GS-memory layout); we keep our own loop and don't call
// glutMainLoop(). Input is intentionally not wired here yet (demo runs without
// it) so glut's pad manager doesn't fight ps2_pad.c.

#include <stdio.h>
#include <string.h>

#include <SDL.h>            // libSDL2main entry + SDL timer
#include <kernel.h>         // FlushCache
#include "GL/gl.h"
#include "GL/glut.h"
#include "GL/ps2gl.h"

extern "C" {
#include "doomkeys.h"
#include "doomgeneric.h"
}

// Doom's palette (CMAP256), from i_video.c.
struct color { uint32_t b:8, g:8, r:8, a:8; };
extern "C" struct color colors[256];

extern "C" void BootScr_Begin(void);
extern "C" void BootScr_End(void);
extern "C" void PS2Audio_Init(void);
extern "C" void I_ResetBaseTime(void);
extern "C" volatile int g_audio_gate;   // i_audsrvsound.c

#ifndef BOOT_LOG_HOLD_MS
#define BOOT_LOG_HOLD_MS 10000
#endif
#define DISP_W 640
#define DISP_H 448

// The GS wants power-of-two textures, so the 320x200 framebuffer is expanded
// into the top-left of a 512x256 RGBA32 texture and the quad samples that region.
#define TEX_W 512
#define TEX_H 256

static int           gl_ready = 0;
static int           g_argc = 0;
static char        **g_argv = 0;
static unsigned int *gl_tex;             // 512x256 RGBA32 framebuffer (DMA mem)
static GLuint        gl_texid;

static void EnsureGl(void)
{
    if (gl_ready)
        return;

    SDL_InitSubSystem(SDL_INIT_TIMER);

    printf("\n>>> build %s %s  (ps2gl video M1, %dx%d) <<<\n",
           __DATE__, __TIME__, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
    SDL_Delay(BOOT_LOG_HOLD_MS);
    I_ResetBaseTime();
    BootScr_End();

    // Bring up ps2gl + the GS (glutInit does GIF reset, SetGsCrt, pglInit and
    // the GS-memory layout). We do NOT call glutMainLoop -- our loop drives.
    glutInit(&g_argc, g_argv);

    gl_tex = (unsigned int *) pglutAllocDmaMem(TEX_W * TEX_H * 4);
    memset(gl_tex, 0, TEX_W * TEX_H * 4);
    glGenTextures(1, &gl_texid);
    glBindTexture(GL_TEXTURE_2D, gl_texid);
    // RGBA32, not paletted: the CPU expands Doom's 8-bit framebuffer through the
    // palette each frame (see DG_DrawFrame). This sidesteps every paletted-texture
    // pitfall on ps2gl -- per-frame CLUT upload, the CSM1 CLUT swizzle, and glut's
    // tiny 2-page PSMT8 slot (which silently promotes an 8-bit image into a 32-bit
    // slot) -- and a 512x256 RGBA area is exactly 64 GS pages = one of glut's
    // 64-page kPsm32 texture slots. RGBA also lets us bilinear-filter safely (no
    // index interpolation), for a smooth upscale.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);

    gl_ready = 1;
    g_audio_gate = 1;   // GS now shows the game -> start audio in sync
}

extern "C" void DG_Init(void) { /* ps2gl is brought up lazily in EnsureGl */ }

extern "C" void DG_DrawFrame(void)
{
    static int first = 1;
    int i, y;

    EnsureGl();

    // *** Why this wait comes FIRST ***
    // ps2gl does NOT copy texture data: glTexImage2D just stores a pointer to
    // our gl_tex, and the upload is a DMA *Ref* to that buffer embedded in the
    // render packet (CImageUploadPkt::Ref(pImage)). The GS reads it throughout
    // the async render kicked by the PREVIOUS pglRenderGeometry(). If we
    // overwrite gl_tex while that DMA is still reading it, the upload gets
    // half-old/half-new bytes -> per-pixel speckle. pglFinishRenderingGeometry()
    // blocks on the GS end-of-render signal, which fires only after that upload
    // DMA has fully drained gl_tex -- so once it returns, gl_tex is safe to
    // rewrite. (pglFinish() is NOT usable here -- it tears down the context.)
    // The blit is trivial, so stalling the EE on the GS costs nothing.
    if (!first) pglFinishRenderingGeometry(PGL_DONT_FORCE_IMMEDIATE_STOP);
    else        first = 0;

    // Previous render is complete -- safe to rewrite the texture. Expand Doom's
    // 320x200 8-bit framebuffer through the palette into the top-left of the
    // 512-wide RGBA32 texture. GS RGBA32 word = R | G<<8 | B<<16 | A<<24
    // (A=0x80 = opaque) -- the byte order the GS expects for a PSMCT32 texel.
    for (y = 0; y < DOOMGENERIC_RESY; ++y)
    {
        const unsigned char *src = (const unsigned char *) DG_ScreenBuffer
                                 + y * DOOMGENERIC_RESX;
        unsigned int *dst = gl_tex + y * TEX_W;
        for (i = 0; i < DOOMGENERIC_RESX; ++i)
        {
            struct color c = colors[src[i]];
            dst[i] = (unsigned int) c.r
                   | ((unsigned int) c.g << 8)
                   | ((unsigned int) c.b << 16)
                   | (0x80u << 24);
        }
    }

    // Flush gl_tex from the CPU data cache so the upload DMA reads the bytes we
    // just wrote, not stale cached RAM. (Necessary but, alone, not sufficient --
    // the wait above is what actually closes the DMA-vs-CPU race.)
    FlushCache(0);

    // Point the texture object at the freshly written buffer (no upload yet --
    // the DMA Ref is emitted when the geometry below is rendered).
    glBindTexture(GL_TEXTURE_2D, gl_texid);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEX_W, TEX_H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, gl_tex);

    // Record the fullscreen textured quad (this embeds the gl_tex Ref into the
    // render packet).
    pglBeginGeometry();
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, DISP_W, DISP_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, gl_texid);
    {
        float u = (float) DOOMGENERIC_RESX / TEX_W;
        float v = (float) DOOMGENERIC_RESY / TEX_H;
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex3f(0,            0,            0);
        glTexCoord2f(u, 0); glVertex3f((float)DISP_W, 0,           0);
        glTexCoord2f(u, v); glVertex3f((float)DISP_W, (float)DISP_H, 0);
        glTexCoord2f(0, v); glVertex3f(0,            (float)DISP_H, 0);
        glEnd();
    }
    glDisable(GL_TEXTURE_2D);
    pglEndGeometry();

    // Flip and kick this frame's render (async; the upload DMA reads the
    // gl_tex/gl_clut we just wrote). We wait for it at the top of the NEXT frame.
    pglWaitForVSync();
    pglSwapBuffers();
    pglRenderGeometry();
}

extern "C" void DG_SleepMs(uint32_t ms)   { SDL_Delay(ms); }
extern "C" uint32_t DG_GetTicksMs(void)   { return SDL_GetTicks(); }
extern "C" void DG_SetWindowTitle(const char *t) { (void) t; }
extern "C" int  DG_GetKey(int *pressed, unsigned char *key) { (void)pressed; (void)key; return 0; }  // M1: no input yet

extern "C" int main(int argc, char **argv)
{
    g_argc = argc;
    g_argv = argv;

    BootScr_Begin();
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("\n=== doomgeneric for PlayStation 2 (ps2gl video, M1) ===\n");

    PS2Audio_Init();
    doomgeneric_Create(argc, argv);

    for (;;)
        doomgeneric_Tick();

    return 0;
}
