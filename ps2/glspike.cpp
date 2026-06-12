// ps2gl spike -- validate the hardware (VU1 + GS) renderer path before
// committing to a full GL Doom renderer.
//
// Draws a PALETTED texture (8-bit indices + a 256-entry CLUT, exactly like
// Doom's textures) on full-screen quads, redrawn several times per frame
// (overdraw) and scrolling, with an on-screen fps counter. It answers:
//   * does ps2gl initialise + render on our setup?
//   * do paletted textures actually work (glColorTable + GL_COLOR_INDEX)?
//   * is the GS fill rate fast enough at this resolution for a hi-res Doom?
//
// Uses the ps2glut framework (entry + GS setup + frame loop), like the stock
// ps2gl examples. Standalone -- not part of the game build.

#include <stdio.h>
#include <string.h>

#include "GL/gl.h"
#include "GL/glut.h"
#include "GL/ps2gl.h"
#include "text_stuff.h"

#include <timer.h>   // cpu_ticks(), kBUSCLK -- glutGet(GLUT_ELAPSED_TIME) returns 0 here

#define TW      64
#define TH      64
#define LAYERS  6     // times the whole screen is re-filled per frame (overdraw)

static GLuint       tex;
static unsigned int palette[256];
static int scr_w = 640, scr_h = 448;
static int frames = 0, fps = 0;
static unsigned int last_ticks = 0, anim = 0;

static void init(void)
{
    int x, y, i;
    unsigned char *pix;

    // Colourful 256-entry RGBA palette so a correct index->colour mapping is
    // visually obvious (a smooth hue ramp).
    for (i = 0; i < 256; i++) {
        unsigned int r = (unsigned int) i;
        unsigned int g = (unsigned int) (255 - i);
        unsigned int b = (unsigned int) ((i * 3) & 0xff);
        palette[i] = r | (g << 8) | (b << 16) | (0x80u << 24);
    }

    // 64x64 8-bit indexed texture (a plasma pattern spanning the palette).
    pix = (unsigned char *) pglutAllocDmaMem(TW * TH);
    for (y = 0; y < TH; y++)
        for (x = 0; x < TW; x++)
            pix[y * TW + x] = (unsigned char) (((x * 4) ^ (y * 4)) & 0xff);

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, 3, TW, TH, 0,
                 GL_COLOR_INDEX, GL_UNSIGNED_BYTE, pix);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);   // tile, don't clamp
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    tsLoadFont();

    glClearColor(0.0f, 0.0f, 0.15f, 0.0f);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);

    printf("glspike: GS VRAM after init:\n");
    pglPrintGsMemAllocation();

    last_ticks = cpu_ticks();
}

static void reshape(int w, int h)
{
    scr_w = w;
    scr_h = h;
    glViewport(0, 0, w, h);
}

static void display(void)
{
    int          layer;
    unsigned int now, dt;
    float        scroll, uw, vh;
    char         buf[96];

    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, scr_w, scr_h, 0, -1, 1);     // 2D, top-left origin
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    scroll = (float) (anim++ & 255) / 256.0f * (float) TW;

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glColorTable(GL_COLOR_TABLE, GL_RGBA, 256, GL_RGBA,
                 GL_UNSIGNED_INT_8_8_8_8, palette);

    uw = (float) scr_w / TW;
    vh = (float) scr_h / TH;
    for (layer = 0; layer < LAYERS; layer++) {
        float s = (scroll + layer * 6.0f) / TW;
        glBegin(GL_QUADS);
        glTexCoord2f(s,      0);  glVertex3f(0,            0,            0);
        glTexCoord2f(s + uw, 0);  glVertex3f((float) scr_w, 0,           0);
        glTexCoord2f(s + uw, vh); glVertex3f((float) scr_w, (float) scr_h, 0);
        glTexCoord2f(s,      vh); glVertex3f(0,            (float) scr_h, 0);
        glEnd();
    }
    glDisable(GL_TEXTURE_2D);

    frames++;
    now = cpu_ticks();
    dt  = now - last_ticks;                       // u32 modular; fine over ~1 s
    if (dt >= (unsigned int) kBUSCLK) {           // ~1 second of EE bus-clock ticks
        fps = (int) (((unsigned long long) frames * (unsigned int) kBUSCLK) / dt);
        frames = 0;
        last_ticks = now;
    }

    tsResetCursor();
    sprintf(buf, "ps2gl spike  %dx%d\nfps: %d\npaletted texture, %d-layer overdraw\n",
            scr_w, scr_h, fps, LAYERS);
    tsDrawString(buf);

    glFlush();
    glutSwapBuffers();
    glutPostRedisplay();
}

int main(int argc, char **argv)
{
    glutInit(&argc, argv);
    init();
    glutReshapeFunc(reshape);
    glutDisplayFunc(display);
    glutMainLoop();
    return 0;
}
