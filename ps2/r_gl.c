// doomgeneric PS2 -- GL world renderer (M2), step 1: walls as geometry.
//
// This is the start of the real hardware renderer that will replace Doom's
// software 3D path. It reads the SAME map data and player viewpoint the
// software renderer uses (the globals R_SetupFrame fills each frame) and emits
// the world as GL geometry for the GS/VU1 to rasterise at high resolution.
//
// M2.1 scope: WALLS ONLY, untextured (flat-shaded by sector light). No flats,
// sky, sprites, lighting model, or UI yet.
//
// IMPORTANT ps2gl rule (learned the hard way -- it hung the GS): render state
// and the projection matrix must be set ONCE, OUTSIDE the per-frame
// pglBeginGeometry/pglEndGeometry block (see RGL_Init), exactly like the ps2gl
// 'box' example does in init()/reshape(). Enabling the depth test or changing
// the projection *inside* the geometry block makes ps2gl reselect its VU1
// renderer mid-packet -> malformed render chain -> the GS never signals
// completion -> pglFinishRenderingGeometry hangs forever (black screen). The
// per-frame RGL_DrawWorld() therefore only sets the modelview and emits verts.
//
// Plain C on purpose: ps2gl's GL/gl.h is extern "C", so we can call gl* from C
// and include Doom's C headers natively (no extern "C" wrapping), then link the
// whole ELF with g++ (ps2gl is C++). See ps2/Makefile (GL_VIDEO).

#include "doomdef.h"
#include "m_fixed.h"
#include "tables.h"
#include "r_defs.h"
#include "r_state.h"   // viewx/viewy/viewz/viewangle + segs/sectors/... externs

#include "GL/gl.h"

// The world is displayed at 4:3 regardless of the GS framebuffer's pixel grid,
// so the projection aspect is 4:3 (not 640/448). 90 deg horizontal FOV.
#define RGL_ASPECT   (4.0f / 3.0f)
#define RGL_ZNEAR    1.0f
#define RGL_ZFAR     65536.0f

// fixed_t (16.16) -> float
#define FX(v) ((float)(v) * (1.0f / 65536.0f))

// angle_t (BAM, full circle = 2^32) -> degrees
#define ANG_DEG(a) ((float)((double)(unsigned int)(a) * (360.0 / 4294967296.0)))

// One-time setup. MUST be called outside any pglBeginGeometry/EndGeometry block
// (we call it from EnsureGl, once, after glutInit brings ps2gl up). Sets the
// render state and the (constant) perspective projection.
void RGL_Init(void)
{
    float w = RGL_ZNEAR;                 // tan(90/2) = 1 -> 90 deg horizontal FOV
    float h = w / RGL_ASPECT;

    glClearColor(0.06f, 0.06f, 0.10f, 0.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);             // M2.1: draw walls double-sided
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-w, w, -h, h, RGL_ZNEAR, RGL_ZFAR);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

// Emit one wall quad spanning v1->v2 horizontally, bot->top vertically. Vertex
// order matches the (Doom.x, Doom.z, -Doom.y) -> GL (x=east, y=up, z=south) map.
static void RGL_Wall(const vertex_t *v1, const vertex_t *v2,
                     fixed_t bot, fixed_t top)
{
    float x1 = FX(v1->x), z1 = -FX(v1->y);
    float x2 = FX(v2->x), z2 = -FX(v2->y);
    float yb = FX(bot),   yt = FX(top);

    if (top <= bot)
        return;

    glVertex3f(x1, yb, z1);
    glVertex3f(x2, yb, z2);
    glVertex3f(x2, yt, z2);
    glVertex3f(x1, yt, z1);
}

// Per-frame: set only the modelview (camera) and emit geometry. No state or
// projection changes here (see RGL_Init / the ps2gl hang note above).
void RGL_DrawWorld(void)
{
    float yaw;

    // No map loaded yet (title/menu screens): render nothing. This also avoids
    // an empty glBegin(GL_QUADS)/glEnd() (zero vertices), which ps2gl may not
    // build into a valid render packet.
    if (numsegs <= 0 || segs == NULL)
        return;

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    // Doom world is X=east, Y=north, Z=up (right-handed); we feed vertices as
    // (x, z, -y) so GL is X=east, Y=up, Z=south (still right-handed). Yaw the
    // world by (90 - viewangle) about the up axis to put the view onto GL -Z.
    yaw = 90.0f - ANG_DEG(viewangle);
    glRotatef(yaw, 0.0f, 1.0f, 0.0f);
    glTranslatef(-FX(viewx), -FX(viewz), FX(viewy));

    // Iterate every seg (no BSP culling yet -- the depth buffer resolves
    // overdraw). frontsector/backsector are NULL-safe checked.
    glBegin(GL_QUADS);
    for (int i = 0; i < numsegs; ++i)
    {
        const seg_t *seg = &segs[i];
        const sector_t *front = seg->frontsector;
        const sector_t *back  = seg->backsector;
        float c;

        if (front == NULL)
            continue;

        c = front->lightlevel * (1.0f / 255.0f);
        if (c < 0.15f) c = 0.15f;

        if (back == NULL)
        {
            glColor3f(c, c, c);
            RGL_Wall(seg->v1, seg->v2, front->floorheight, front->ceilingheight);
        }
        else
        {
            if (front->ceilingheight > back->ceilingheight)
            {
                glColor3f(c * 0.80f, c * 0.80f, c * 0.92f);
                RGL_Wall(seg->v1, seg->v2,
                         back->ceilingheight, front->ceilingheight);
            }
            if (back->floorheight > front->floorheight)
            {
                glColor3f(c * 0.92f, c * 0.86f, c * 0.78f);
                RGL_Wall(seg->v1, seg->v2,
                         front->floorheight, back->floorheight);
            }
        }
    }
    glEnd();
    glColor3f(1.0f, 1.0f, 1.0f);
}
