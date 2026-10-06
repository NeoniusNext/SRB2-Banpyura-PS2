/* Execute real batching code against a synchronous triangle recorder. The Python runner compares
 * every triangle's vertices and effective state with the pre-optimization implementation. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define HWRENDER
#define PS2_PROFILE
#define _HWR_GLOB_H_
#define __HWR_BATCHING_H__
#define __I_SYSTEM__
#define __Z_ZONE__
typedef int boolean;
typedef int32_t INT32;
typedef uint32_t UINT32;
typedef unsigned long FUINT;
typedef unsigned long FBITFIELD;
#define true 1
#define false 0
#define PF_NoTexture 1u
#define SHADER_NONE -1
#define PU_HWRBATCH 25
typedef struct { float x, y, z, s, t; } FOutVector;
typedef struct { UINT32 rgba; } color_t;
typedef struct { FUINT light_level, fade_start, fade_end; } light_t;
typedef struct { FUINT PolyFlags; color_t PolyColor, TintColor, FadeColor; UINT32 LightTableId; light_t LightInfo; } FSurfaceInfo;
typedef struct { UINT32 downloaded; } GLMipmap_t;
typedef struct {
    FSurfaceInfo surf; unsigned int vertsIndex; FUINT numVerts; FBITFIELD polyFlags;
    GLMipmap_t *texture; int shader; boolean horizonSpecial; INT32 hash;
} PolygonArrayEntry;
typedef struct { union { int i; } value; } stat_t;
static stat_t ps_hw_numpolys, ps_hw_numcalls, ps_hw_numshaders, ps_hw_numtextures,
    ps_hw_numpolyflags, ps_hw_numcolors, ps_hw_numverts;
#define PS_START_TIMING(x) ((void)0)
#define PS_STOP_TIMING(x) ((void)0)
static struct { int value; } cv_glshaders = {1};
static int gl_shadersavailable = 1;
static int HWR_GetShaderFromTarget(int target) { return target + 100; }
static void I_Error(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); exit(2);
}
#ifdef PS2
/* Budget/resize correctness is tested with the actual zone in zone_hosttest; this fixture records geometry. */
static void *Z_ReallocAlign(void *p, size_t bytes, int tag, void *owner, int align)
{
    assert(tag == PU_HWRBATCH && !owner && align == 4);
    return realloc(p, bytes);
}
#endif
static GLMipmap_t *selected_texture;
static int selected_shader;
static uint64_t digest = 1469598103934665603ull, triangles;
static void mix(const void *data, size_t size)
{
    const unsigned char *p = data;
    while (size--) { digest ^= *p++; digest *= 1099511628211ull; }
}
static void texture(GLMipmap_t *p) { selected_texture = p; }
static void shader(int s) { selected_shader = s; }
static void indexed(FSurfaceInfo *surf, FOutVector *verts, FUINT count, FBITFIELD flags, UINT32 *indices)
{
    FUINT i;
    UINT32 tex = (flags & PF_NoTexture) || !selected_texture ? 0 : selected_texture->downloaded;
    assert(count % 3 == 0);
    for (i = 0; i < count; i += 3)
    {
        mix(&tex, sizeof tex); mix(&selected_shader, sizeof selected_shader);
        mix(&flags, sizeof flags); mix(surf, sizeof *surf);
        mix(&verts[indices[i]], sizeof *verts); mix(&verts[indices[i+1]], sizeof *verts);
        mix(&verts[indices[i+2]], sizeof *verts); triangles++;
    }
}
static void polygon(FSurfaceInfo *s, FOutVector *v, FUINT n, FBITFIELD f)
{ (void)s; (void)v; (void)n; (void)f; assert(0); }
static struct {
    void (*pfnSetTexture)(GLMipmap_t *); void (*pfnSetShader)(int);
    void (*pfnDrawPolygon)(FSurfaceInfo *, FOutVector *, FUINT, FBITFIELD);
    void (*pfnDrawIndexedTriangles)(FSurfaceInfo *, FOutVector *, FUINT, FBITFIELD, UINT32 *);
} HWD = {texture, shader, polygon, indexed};
#include MEMORY_BATCH_SOURCE

int main(void)
{
    GLMipmap_t textures[7];
    FSurfaceInfo surf;
    FOutVector *v = calloc(10000, sizeof *v);
    int frame, p, j;
    uint64_t expected = 0;
    assert(v);
    for (j = 0; j < 7; j++) textures[j].downloaded = (UINT32)j + 1;
    for (frame = 0; frame < 6; frame++)
    {
        cv_glshaders.value = frame % 2;
        HWR_StartBatching();
        for (p = 0; p < (frame == 0 ? 0 : 6000); p++)
        {
            int n = frame == 5 && p == 123 ? 9000 : 3 + p % 13;
            int state = frame == 1 ? 0 : (p * 113) % 7;
            FBITFIELD flags = frame == 4 && p % 17 == 0 ? PF_NoTexture : 0;
            memset(&surf, 0, sizeof surf);
            surf.PolyColor.rgba = (UINT32)state * 391u;
            surf.TintColor.rgba = (UINT32)state + 9;
            surf.FadeColor.rgba = 42;
            surf.LightInfo.light_level = (FUINT)(state + 3);
            surf.LightInfo.fade_end = 300;
            for (j = 0; j < n; j++)
            {
                v[j].x = (float)(p * 17 + j); v[j].y = (float)(frame * 9);
                v[j].z = (float)j; v[j].s = (float)(j % 7); v[j].t = (float)(p % 11);
            }
            HWR_SetCurrentTexture(&textures[state]);
            HWR_ProcessPolygon(&surf, v, (FUINT)n, flags, state, frame == 3 && p < 8);
            expected += (uint64_t)n - 2;
        }
        HWR_RenderBatches();
        assert(triangles == expected && !polygonArraySize && !unsortedVertexArraySize);
    }
#ifdef PS2
    assert(!finalVertexArray); // the entire duplicate vertex array is gone on the EE
    assert(finalVertexArrayAllocSize == 12288); // only the single large fan grows the bounded scratch
#endif
    printf("STREAM triangles=%llu digest=%016llx\n", (unsigned long long)triangles, (unsigned long long)digest);
    free(v);
    return 0;
}
