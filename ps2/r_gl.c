// doomgeneric PS2 -- world renderer, Doom side (visibility + textures + camera).
//
// Reads Doom's per-frame view + map data and feeds visible, textured walls to
// the native GS/VU1 pipeline in r_native.c via the float/buffer-only RN_* API.
// Includes Doom's headers (seg_t/sector_t/textures/palette) but NOT PS2SDK's
// draw.h, which would clash (both define vertex_t / line_t) -- the two halves are
// deliberately split.
//
// Visibility = Doom's own BSP walk (R_Subsector hook marks every seg of every
// visited subsector); the GS Z-buffer in r_native does occlusion, so grazing
// walls Doom's 320-column software clip would drop are kept. Textures: each Doom
// wall texture is palette-expanded to a small RGBA tile, uploaded to its own VRAM
// slot once, then walls are drawn batched per texture and modulated by sector
// light.

#include "doomdef.h"
#include "m_fixed.h"
#include "tables.h"
#include "r_defs.h"
#include "r_state.h"   // viewx/y/z/viewangle, segs/sectors/sides/... externs
#include "r_data.h"    // R_GetColumn
#include "w_wad.h"     // W_CacheLumpNum (flats)
#include "z_zone.h"    // PU_CACHE

#include <stdlib.h>
#include <string.h>
#include <math.h>

// Doom texture globals (struct color + colors[] come from i_video.h via r_defs.h).
extern int      numtextures;
extern int*     texturewidthmask;   // (pow2 width) - 1
extern fixed_t* textureheight;       // height << FRACBITS
extern int      firstflat;           // lump number of the first flat
extern int      skyflatnum;          // flat index meaning "sky"
extern gamestate_t    gamestate;       // GS_LEVEL etc. (doomdef.h enum)
extern boolean        menuactive;      // menu open (overlays even during a level)
extern unsigned char* DG_ScreenBuffer; // 320x200 8-bit UI framebuffer (CMAP256)

// Native pipeline (r_native.c) -- no Doom/PS2SDK type clash.
extern void RN_Init(void);
extern void RN_FrameBegin(float camx, float camy, float camz, float yaw);
extern void RN_FrameEnd(void);
extern void RN_AddWall(float x1, float z1, float x2, float z2,
                       float yb, float yt, float u0, float u1, float v);
extern void RN_AddTri(float ax, float ay, float az, float as, float at,
                      float bx, float by, float bz, float bs, float bt,
                      float cx, float cy, float cz, float cs, float ct);
extern void RN_SetLight(int l);
extern int  RN_TexCreate(void);
extern void RN_TexUpload(int h, const void* rgba);
extern void RN_TexBind(int h);
extern void RN_Draw2D(const void* rgba, int w, int h);

#define FX(v)      ((float)(v) * (1.0f / 65536.0f))
#define ANG_RAD(a) ((float)((double)(unsigned int)(a) * (6.283185307179586 / 4294967296.0)))
#define RGL_PI     3.14159265358979f
#define RGL_TEXD   64                  // must match r_native TEX_DIM
#define NEARW      4.0f                // world near-clip distance (> projection near=2)

// View basis, set each frame; used by the wall near-clip and the cull.
static float s_cosv, s_sinv, s_fvx, s_fvy;

// ---------------------------------------------------------------------------
// Visibility marks (filled by the BSP hooks each frame) + visible-seg list.
// ---------------------------------------------------------------------------
static unsigned char* seg_vis;
static int            seg_vis_cap;
static int*           vis_list;
static int            vis_cap;

void RGL_BeginVis(void)
{
    if (seg_vis_cap < numsegs) {
        seg_vis  = (unsigned char*) realloc(seg_vis, (size_t)numsegs);
        vis_list = (int*) realloc(vis_list, (size_t)numsegs * sizeof(int));
        seg_vis_cap = (seg_vis && vis_list) ? numsegs : 0;
        vis_cap     = seg_vis_cap;
    }
    if (seg_vis)
        memset(seg_vis, 0, (size_t)numsegs);
}

void RGL_MarkSeg(seg_t* seg, int level)
{
    int i;
    (void)level;
    if (!seg_vis || segs == NULL)
        return;
    i = (int)(seg - segs);
    if (i >= 0 && i < numsegs)
        seg_vis[i] = 1;
}

// ---------------------------------------------------------------------------
// Texture cache: Doom texture number -> VRAM handle (-2 not tried, -1 failed).
// ---------------------------------------------------------------------------
static short* texhandle;
static int    texhandle_cap;
static unsigned int texrgba[RGL_TEXD * RGL_TEXD] __attribute__((aligned(64)));
static unsigned int ui_rgba[320 * 200] __attribute__((aligned(128)));   // 2D UI staging

static int ensure_tex(int tn)
{
    int w, sh, col, row, h;

    if (tn <= 0 || tn >= numtextures)
        return -1;
    if (texhandle_cap < numtextures) {
        int old = texhandle_cap, i;
        texhandle = (short*) realloc(texhandle, (size_t)numtextures * sizeof(short));
        texhandle_cap = texhandle ? numtextures : 0;
        for (i = old; i < texhandle_cap; ++i)
            texhandle[i] = -2;
    }
    if (!texhandle)
        return -1;
    if (texhandle[tn] != -2)
        return texhandle[tn];

    w  = texturewidthmask[tn] + 1;
    sh = (int)(textureheight[tn] >> FRACBITS);
    if (sh <= 0) { texhandle[tn] = -1; return -1; }

    for (col = 0; col < RGL_TEXD; ++col) {
        int scol = (col * w) / RGL_TEXD;
        const unsigned char* src = (const unsigned char*) R_GetColumn(tn, scol);
        for (row = 0; row < RGL_TEXD; ++row) {
            int srow = ((row * sh) / RGL_TEXD) % sh;
            struct color c = colors[src[srow]];
            texrgba[row * RGL_TEXD + col] = (unsigned int)c.r
                                          | ((unsigned int)c.g << 8)
                                          | ((unsigned int)c.b << 16)
                                          | (0x80u << 24);
        }
    }

    h = RN_TexCreate();
    if (h < 0) { texhandle[tn] = -1; return -1; }
    RN_TexUpload(h, texrgba);
    texhandle[tn] = (short)h;
    return h;
}

// ---------------------------------------------------------------------------
// Flat cache: Doom flat number -> VRAM handle. Flats are 64x64 raw indices.
// ---------------------------------------------------------------------------
static short* flathandle;
static int    flathandle_cap;

static int ensure_flat(int fn)
{
    const unsigned char* data;
    int row, col, h, i;

    if (fn < 0)
        return -1;
    if (flathandle_cap <= fn) {
        int old = flathandle_cap, nc = fn + 16;
        flathandle = (short*) realloc(flathandle, (size_t)nc * sizeof(short));
        flathandle_cap = flathandle ? nc : 0;
        for (i = old; i < flathandle_cap; ++i)
            flathandle[i] = -2;
    }
    if (!flathandle)
        return -1;
    if (flathandle[fn] != -2)
        return flathandle[fn];

    data = (const unsigned char*) W_CacheLumpNum(firstflat + fn, PU_CACHE);  // 64x64
    for (row = 0; row < RGL_TEXD; ++row)
        for (col = 0; col < RGL_TEXD; ++col) {
            int sr = (row * 64) / RGL_TEXD, sc = (col * 64) / RGL_TEXD;
            struct color c = colors[data[(sr & 63) * 64 + (sc & 63)]];
            texrgba[row * RGL_TEXD + col] = (unsigned int)c.r
                                          | ((unsigned int)c.g << 8)
                                          | ((unsigned int)c.b << 16)
                                          | (0x80u << 24);
        }
    h = RN_TexCreate();
    if (h < 0) { flathandle[fn] = -1; return -1; }
    RN_TexUpload(h, texrgba);
    flathandle[fn] = (short)h;
    return h;
}

// ---------------------------------------------------------------------------
// Subsector floor/ceiling polygons. Doom stores only a subsector's seg (wall)
// edges, not the implicit BSP-partition edges, so fan-triangulating the segs
// leaves HOLES in open areas. We reconstruct each subsector's true convex
// polygon once per level by clipping the map bounding box down the BSP tree:
// each node's partition line splits the running polygon between its front/back
// children, and the leaf receives its exact convex cell.
// ---------------------------------------------------------------------------
extern node_t* nodes;
extern int     numnodes;

#define MAXPOLYV 64                      // max verts of a reconstructed subsector cell
typedef struct { float x, y; } pt_t;     // Doom map-unit coords (FX-applied)

static pt_t*          sub_poly;          // MAXPOLYV verts per subsector
static unsigned char* sub_pn;            // vert count per subsector
static void*          poly_built_ptr;    // subsectors ptr the polys were built for

static int clip_hp(const pt_t* in, int n, pt_t* out,
                   float nx, float ny, float ndx, float ndy, int keepFront)
{
    int i, m = 0;
    for (i = 0; i < n; ++i) {
        const pt_t* A = &in[i];
        const pt_t* B = &in[(i + 1) % n];
        float fa = ndx * (A->y - ny) - ndy * (A->x - nx);   // <0 = Doom front side
        float fb = ndx * (B->y - ny) - ndy * (B->x - nx);
        int inA = keepFront ? (fa <= 0.0f) : (fa >= 0.0f);
        int inB = keepFront ? (fb <= 0.0f) : (fb >= 0.0f);
        if (inA && m < MAXPOLYV) out[m++] = *A;
        if (inA != inB && m < MAXPOLYV) {
            float t = fa / (fa - fb);
            out[m].x = A->x + t * (B->x - A->x);
            out[m].y = A->y + t * (B->y - A->y);
            ++m;
        }
    }
    return m;
}

static void build_sub(int bspnum, pt_t* poly, int np)
{
    node_t* nd;
    pt_t fr[MAXPOLYV], bk[MAXPOLYV];
    int nf, nb;

    if (bspnum & NF_SUBSECTOR) {
        int ss = (bspnum == -1) ? 0 : (bspnum & ~NF_SUBSECTOR);
        if (ss >= 0 && ss < numsubsectors) {
            int c = (np < MAXPOLYV) ? np : MAXPOLYV;
            memcpy(&sub_poly[ss * MAXPOLYV], poly, (size_t)c * sizeof(pt_t));
            sub_pn[ss] = (unsigned char)c;
        }
        return;
    }
    nd = &nodes[bspnum];
    nf = clip_hp(poly, np, fr, FX(nd->x), FX(nd->y), FX(nd->dx), FX(nd->dy), 1);
    nb = clip_hp(poly, np, bk, FX(nd->x), FX(nd->y), FX(nd->dx), FX(nd->dy), 0);
    build_sub(nd->children[0], fr, nf);
    build_sub(nd->children[1], bk, nb);
}

static void ensure_polys(void)
{
    pt_t bbox[4];
    if (poly_built_ptr == (void*)subsectors && sub_poly)
        return;                          // already built for this level
    free(sub_poly); free(sub_pn);
    sub_poly = (pt_t*) malloc((size_t)numsubsectors * MAXPOLYV * sizeof(pt_t));
    sub_pn   = (unsigned char*) calloc((size_t)numsubsectors, 1);
    poly_built_ptr = (void*)subsectors;
    if (!sub_poly || !sub_pn) { sub_poly = NULL; return; }
    bbox[0].x = -32768.0f; bbox[0].y = -32768.0f;
    bbox[1].x =  32768.0f; bbox[1].y = -32768.0f;
    bbox[2].x =  32768.0f; bbox[2].y =  32768.0f;
    bbox[3].x = -32768.0f; bbox[3].y =  32768.0f;
    if (numnodes > 0)
        build_sub(numnodes - 1, bbox, 4);
    else if (numsubsectors > 0) {
        memcpy(&sub_poly[0], bbox, 4 * sizeof(pt_t));
        sub_pn[0] = 4;
    }
}

// ---------------------------------------------------------------------------
void RGL_Init(void)
{
    RN_Init();
}

static void wall(const seg_t* seg, fixed_t bot, fixed_t top, int tn)
{
    float x1 = FX(seg->v1->x), y1 = FX(seg->v1->y);   // Doom map coords
    float x2 = FX(seg->v2->x), y2 = FX(seg->v2->y);
    float yb = FX(bot), yt = FX(top);
    float f1, f2, len, tw, th, u0, u1;

    if (top <= bot)
        return;
    // Forward distance of each endpoint along the view direction.
    f1 = (x1 - s_fvx) * s_cosv + (y1 - s_fvy) * s_sinv;
    f2 = (x2 - s_fvx) * s_cosv + (y2 - s_fvy) * s_sinv;
    if (f1 < NEARW && f2 < NEARW)
        return;                          // wholly behind the near plane

    tw = (float)(texturewidthmask[tn] + 1);
    th = (float)(textureheight[tn] >> FRACBITS);
    if (tw < 1.0f) tw = 64.0f;
    if (th < 1.0f) th = 64.0f;
    { float dx = x2 - x1, dy = y2 - y1; len = sqrtf(dx * dx + dy * dy); }
    u0 = 0.0f; u1 = len / tw;

    // Trim the endpoint behind the near plane to it. The VU DROPS (doesn't clip)
    // near-crossing triangles, so without this a wall vanishes point-blank.
    if (f1 < NEARW) {
        float t = (NEARW - f1) / (f2 - f1);
        x1 += t * (x2 - x1);  y1 += t * (y2 - y1);  u0 = t * (len / tw);
    } else if (f2 < NEARW) {
        float t = (NEARW - f2) / (f1 - f2);
        x2 += t * (x1 - x2);  y2 += t * (y1 - y2);  u1 = (1.0f - t) * (len / tw);
    }
    RN_AddWall(x1, -y1, x2, -y2, yb, yt, u0, u1, (yt - yb) / th);
}

// One flat triangle (map coords, at height h), clipped against the near plane
// before emit -- the VU drops near-crossing tris, which is the near-camera floor
// hole. Flat ST tiles the world every 64 units, so it's derived from x,y.
static void flat_tri(float ax, float ay, float bx, float by,
                     float cx, float cy, float h)
{
    pt_t in[3], out[6];
    int  i, nout = 0;

    in[0].x = ax; in[0].y = ay;
    in[1].x = bx; in[1].y = by;
    in[2].x = cx; in[2].y = cy;
    for (i = 0; i < 3; ++i) {
        const pt_t* A = &in[i];
        const pt_t* B = &in[(i + 1) % 3];
        float fa = (A->x - s_fvx) * s_cosv + (A->y - s_fvy) * s_sinv - NEARW;
        float fb = (B->x - s_fvx) * s_cosv + (B->y - s_fvy) * s_sinv - NEARW;
        if (fa >= 0.0f && nout < 6) out[nout++] = *A;
        if (((fa >= 0.0f) != (fb >= 0.0f)) && nout < 6) {
            float t = fa / (fa - fb);
            out[nout].x = A->x + t * (B->x - A->x);
            out[nout].y = A->y + t * (B->y - A->y);
            ++nout;
        }
    }
    for (i = 1; i + 1 < nout; ++i)
        RN_AddTri(
            out[0].x,   h, -out[0].y,   out[0].x  *(1.0f/64.0f), out[0].y  *(1.0f/64.0f),
            out[i].x,   h, -out[i].y,   out[i].x  *(1.0f/64.0f), out[i].y  *(1.0f/64.0f),
            out[i+1].x, h, -out[i+1].y, out[i+1].x*(1.0f/64.0f), out[i+1].y*(1.0f/64.0f));
}

void RGL_DrawFrame(void)
{
    float yaw;
    int i, k, nvis, tn;

    yaw = ANG_RAD(viewangle) - (RGL_PI * 0.5f);
    RN_FrameBegin(FX(viewx), FX(viewz), -FX(viewy), yaw);

    // Title / menu / intermission / finale -> blit Doom's 2D framebuffer
    // fullscreen. (The demo plays as GS_LEVEL, so it still renders in 3D unless
    // the menu is open over it.)
    if (gamestate != GS_LEVEL || menuactive) {
        if (DG_ScreenBuffer) {
            const unsigned char* src = (const unsigned char*) DG_ScreenBuffer;
            int i;
            for (i = 0; i < 320 * 200; ++i) {
                struct color c = colors[src[i]];
                ui_rgba[i] = (unsigned int)c.r | ((unsigned int)c.g << 8)
                           | ((unsigned int)c.b << 16) | (0x80u << 24);
            }
            RN_Draw2D(ui_rgba, 320, 200);
        }
        RN_FrameEnd();
        return;
    }

    if (numsegs <= 0 || segs == NULL || seg_vis == NULL || vis_list == NULL) {
        RN_FrameEnd();
        return;
    }

    // One cull pass -> compact list of visible (and in-front) segs.
    s_cosv = cosf(ANG_RAD(viewangle));
    s_sinv = sinf(ANG_RAD(viewangle));
    s_fvx  = FX(viewx);
    s_fvy  = FX(viewy);
    nvis = 0;
    for (i = 0; i < numsegs; ++i) {
        const seg_t* seg;
        float f1, f2;
        if (!seg_vis[i])
            continue;
        seg = &segs[i];
        f1 = (FX(seg->v1->x) - s_fvx) * s_cosv + (FX(seg->v1->y) - s_fvy) * s_sinv;
        f2 = (FX(seg->v2->x) - s_fvx) * s_cosv + (FX(seg->v2->y) - s_fvy) * s_sinv;
        if (f1 < 1.0f && f2 < 1.0f)
            continue;
        vis_list[nvis++] = i;
    }

    // Draw batched by texture (so each VRAM texture binds once per frame).
    for (tn = 1; tn < numtextures; ++tn) {
        int bound = 0;
        for (k = 0; k < nvis; ++k) {
            const seg_t*    seg   = &segs[vis_list[k]];
            const side_t*   side  = seg->sidedef;
            const sector_t* front = seg->frontsector;
            const sector_t* back  = seg->backsector;
            int li;

            if (front == NULL || side == NULL)
                continue;
            li = front->lightlevel >> 1;

            if (back == NULL) {
                if (side->midtexture == tn) {
                    if (!bound) { int h = ensure_tex(tn); if (h < 0) break; RN_TexBind(h); bound = 1; }
                    RN_SetLight(li);
                    wall(seg, front->floorheight, front->ceilingheight, tn);
                }
            } else {
                if (side->toptexture == tn && front->ceilingheight > back->ceilingheight) {
                    if (!bound) { int h = ensure_tex(tn); if (h < 0) break; RN_TexBind(h); bound = 1; }
                    RN_SetLight(li);
                    wall(seg, back->ceilingheight, front->ceilingheight, tn);
                }
                if (side->bottomtexture == tn && back->floorheight > front->floorheight) {
                    if (!bound) { int h = ensure_tex(tn); if (h < 0) break; RN_TexBind(h); bound = 1; }
                    RN_SetLight(li);
                    wall(seg, front->floorheight, back->floorheight, tn);
                }
            }
        }
    }

    // Flats: floor + ceiling of every visited subsector, from the reconstructed
    // convex polygon (fills the open-area holes the seg fan left). Floor drawn
    // when the eye is above it, ceiling when below; sky ceilings stay open.
    ensure_polys();
    {
        int s, j;
        for (s = 0; s < numsubsectors; ++s) {
            const subsector_t* sub = &subsectors[s];
            int fl = sub->firstline;
            const sector_t* sec;
            const pt_t* pv;
            int pc, li, h;
            float y;

            if (fl < 0 || fl >= numsegs || !seg_vis[fl] || !sub_pn || sub_pn[s] < 3)
                continue;
            sec = sub->sector;
            if (sec == NULL)
                continue;
            pv = &sub_poly[s * MAXPOLYV];
            pc = sub_pn[s];
            li = sec->lightlevel >> 1;

            #define PV3(a,b,c) flat_tri(pv[a].x, pv[a].y, pv[b].x, pv[b].y, \
                                         pv[c].x, pv[c].y, y)

            if (sec->floorheight < viewz) {
                h = ensure_flat(sec->floorpic);
                if (h >= 0) {
                    RN_TexBind(h); RN_SetLight(li);
                    y = FX(sec->floorheight);
                    for (j = 1; j + 1 < pc; ++j) PV3(0, j, j + 1);
                }
            }
            if (sec->ceilingheight > viewz && sec->ceilingpic != skyflatnum) {
                h = ensure_flat(sec->ceilingpic);
                if (h >= 0) {
                    RN_TexBind(h); RN_SetLight(li);
                    y = FX(sec->ceilingheight);
                    for (j = 1; j + 1 < pc; ++j) PV3(0, j, j + 1);
                }
            }
            #undef PV3
        }
    }

    RN_FrameEnd();
}
