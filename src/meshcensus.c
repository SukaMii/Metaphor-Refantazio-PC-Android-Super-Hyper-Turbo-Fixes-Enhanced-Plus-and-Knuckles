/* [GpuProbe] MeshCensus=1 (diagnostic, PC): which meshes the far world triangles come from, and
 * what simpler versions of them could be made from.
 *
 * The game has no working switch to simpler world models: the layout's LOD records
 * (fldLayoutOfModel_LOD) are loaded but never turned into objects, and the per-node
 * fldLODCameraDist is stored once and never read. Simpler models would have to be
 * made from the game's own meshes. This census measures whether that is worth it.
 *
 * F9 records 300 frames: every indexed scene draw is filed under its mesh (index buffer, first
 * index, index count, base vertex, vertex buffer, input layout) with its triangles per distance
 * band, its on-screen size and the shadow draws of the same mesh. Then MetaphorCPU_meshes_<time>
 * in the game folder gets census.txt (the meshes ranked by triangles beyond 50 m, their buffer and
 * vertex formats, how few meshes make most of the far triangles) and mesh_NNN.bin for the top 40
 * (their indices and vertices, read back from the GPU) for offline simplification tests.
 * Nothing is recorded outside the 300 frames. */
#define COBJMACROS
#include "mcpu.h"
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- buffers and input layouts */
typedef struct {
    void* p;
    UINT usage, bind, width, init;
} BufInfo;
#define BUF_N 65536
static BufInfo g_buf[BUF_N];
typedef struct {
    void* p;
    int n, pos_fmt, pos_off, pos_slot, skinned;
    char desc[160];
} LayInfo;
#define LAY_N 8192
static LayInfo g_lay[LAY_N];
static SRWLOCK g_info_lock = SRWLOCK_INIT;

static size_t ptr_hash(const void* p) { return (size_t)(((uintptr_t)p >> 4) * 0x9E3779B97F4A7C15ull >> 20); }

void census_buffer_created(void* buf, const void* desc, int has_init)
{
    const D3D11_BUFFER_DESC* d = (const D3D11_BUFFER_DESC*)desc;
    if (!buf || !d || !(d->BindFlags & (D3D11_BIND_INDEX_BUFFER | D3D11_BIND_VERTEX_BUFFER)))
        return;
    AcquireSRWLockExclusive(&g_info_lock);
    size_t h = ptr_hash(buf) % BUF_N;
    for (int k = 0; k < 64; k++, h = (h + 1) % BUF_N)
        if (!g_buf[h].p || g_buf[h].p == buf) { /* a freed buffer's address may come back: overwrite */
            g_buf[h] = (BufInfo){buf, d->Usage, d->BindFlags, d->ByteWidth, (UINT)(has_init != 0)};
            break;
        }
    ReleaseSRWLockExclusive(&g_info_lock);
}

static BufInfo buf_info(void* buf)
{
    BufInfo r = {0};
    AcquireSRWLockShared(&g_info_lock);
    size_t h = ptr_hash(buf) % BUF_N;
    for (int k = 0; k < 64 && g_buf[h].p; k++, h = (h + 1) % BUF_N)
        if (g_buf[h].p == buf) {
            r = g_buf[h];
            break;
        }
    ReleaseSRWLockShared(&g_info_lock);
    return r;
}

static UINT fmt_bytes(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT: return 16;
    case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R32G32B32_SINT: return 12;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT: return 8;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT: case DXGI_FORMAT_R11G11B10_FLOAT: return 4;
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R8G8_UNORM: return 2;
    default: return 4;
    }
}

void census_layout_created(void* lay, const void* elems, unsigned n)
{
    const D3D11_INPUT_ELEMENT_DESC* e = (const D3D11_INPUT_ELEMENT_DESC*)elems;
    if (!lay || !e)
        return;
    LayInfo li = {lay, (int)n, 0, -1, 0, 0, ""};
    UINT next[16] = {0};
    size_t dl = 0;
    for (unsigned i = 0; i < n; i++) {
        UINT slot = e[i].InputSlot < 16 ? e[i].InputSlot : 0;
        UINT off = e[i].AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT ? next[slot] : e[i].AlignedByteOffset;
        next[slot] = off + fmt_bytes(e[i].Format);
        const char* s = e[i].SemanticName ? e[i].SemanticName : "?";
        if (!_stricmp(s, "POSITION") && e[i].SemanticIndex == 0)
            li.pos_fmt = e[i].Format, li.pos_off = (int)off, li.pos_slot = (int)slot;
        if (!_stricmp(s, "BLENDINDICES") || !_stricmp(s, "BLENDWEIGHT"))
            li.skinned = 1;
        if (dl < sizeof(li.desc) - 24)
            dl += (size_t)snprintf(li.desc + dl, sizeof(li.desc) - dl, "%s%.6s%u@%u/%u:%d", i ? " " : "", s,
                                   e[i].SemanticIndex, slot, off, (int)e[i].Format);
    }
    AcquireSRWLockExclusive(&g_info_lock);
    size_t h = ptr_hash(lay) % LAY_N;
    for (int k = 0; k < 64; k++, h = (h + 1) % LAY_N)
        if (!g_lay[h].p || g_lay[h].p == lay) {
            g_lay[h] = li;
            break;
        }
    ReleaseSRWLockExclusive(&g_info_lock);
}

static LayInfo lay_info(void* lay)
{
    LayInfo r = {0};
    r.pos_slot = -1;
    AcquireSRWLockShared(&g_info_lock);
    size_t h = ptr_hash(lay) % LAY_N;
    for (int k = 0; k < 64 && g_lay[h].p; k++, h = (h + 1) % LAY_N)
        if (g_lay[h].p == lay) {
            r = g_lay[h];
            break;
        }
    ReleaseSRWLockShared(&g_info_lock);
    return r;
}

/* the position of an input layout (also for farmesh.c): 0 if the layout was not seen created */
int mesh_layout_pos(void* lay, int* fmt, int* off, int* slot, int* skinned)
{
    LayInfo li = lay_info(lay);
    if (!li.p)
        return 0;
    *fmt = li.pos_fmt, *off = li.pos_off, *slot = li.pos_slot, *skinned = li.skinned;
    return 1;
}

/* ---------------------------------------------------------------- the census */
#define BANDS 5 /* <15, 15-30, 30-50, 50-100, >100 m (as the triangle accounting) */
typedef struct {
    void *ib, *vb, *lay; /* held (AddRef) until the census is written */
    UINT start, count, ib_fmt, ib_off, vb_stride, vb_off;
    INT base;
    LONG64 tris[2 * BANDS]; /* world, then characters */
    LONG64 draws, inst_draws, shadow_tris;
    double far_area, far_tris_area; /* on-screen pixels and triangles of the draws beyond 50 m */
    float dmin, dmax;
} Mesh;
#define MESH_N 32768
static Mesh* g_mesh;
static LONG g_nmesh, g_full;
static SRWLOCK g_mesh_lock = SRWLOCK_INIT;
static volatile LONG g_rec; /* recording */
static int g_rec_frames, g_rec_left;
static LONG64 g_unknown_tris; /* scene draws whose buffers could not be read */

int census_recording(void) { return g_rec; }

static size_t mesh_hash(void* ib, UINT start, UINT count, INT base, void* vb)
{
    uint64_t h = (uintptr_t)ib * 0x9E3779B97F4A7C15ull;
    h ^= ((uint64_t)start << 32 | count) * 0xC2B2AE3D27D4EB4Full;
    h ^= ((uint64_t)(uint32_t)base * 0x165667B19E3779F9ull) ^ (uintptr_t)vb * 0x27D4EB2F165667C5ull;
    return (size_t)(h >> 17) % MESH_N;
}

/* an indexed draw while recording: cls = the scene object's class (world / characters x band),
 * -1 outside scene objects; shadow = drawn into a shadow cascade */
void census_draw(void* ctx, unsigned count, unsigned inst, unsigned start, int base, int cls, float area, float dist_m,
                 int shadow)
{
    if (!g_rec || !g_mesh || (cls < 0 && !shadow) || cls >= 2 * BANDS)
        return;
    ID3D11DeviceContext* c = (ID3D11DeviceContext*)ctx;
    ID3D11Buffer *ib = NULL, *vb = NULL;
    ID3D11InputLayout* lay = NULL;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    UINT ioff = 0, stride = 0, voff = 0;
    ID3D11DeviceContext_IAGetIndexBuffer(c, &ib, &fmt, &ioff);
    ID3D11DeviceContext_IAGetVertexBuffers(c, 0, 1, &vb, &stride, &voff);
    ID3D11DeviceContext_IAGetInputLayout(c, &lay);
    LONG64 tris = (LONG64)(count / 3) * (inst ? inst : 1);
    if (!ib || !vb) {
        if (cls >= 0)
            InterlockedAdd64(&g_unknown_tris, tris);
    } else {
        AcquireSRWLockExclusive(&g_mesh_lock);
        size_t h = mesh_hash(ib, start, count, base, vb);
        Mesh* m = NULL;
        for (int k = 0; k < 256; k++, h = (h + 1) % MESH_N) {
            Mesh* e = &g_mesh[h];
            if (!e->ib) {
                if (cls < 0) /* shadow draws only add to meshes the scene draws */
                    break;
                e->ib = ib, e->vb = vb, e->lay = lay, e->start = start, e->count = count, e->base = base;
                e->ib_fmt = fmt, e->ib_off = ioff, e->vb_stride = stride, e->vb_off = voff;
                e->dmin = 1e9f, e->dmax = 0;
                ID3D11Buffer_AddRef(ib);
                ID3D11Buffer_AddRef(vb);
                if (lay)
                    ID3D11InputLayout_AddRef(lay);
                g_nmesh++;
                m = e;
                break;
            }
            if (e->ib == ib && e->start == start && e->count == count && e->base == base && e->vb == vb) {
                m = e;
                break;
            }
        }
        if (m && cls >= 0) {
            m->tris[cls] += tris;
            m->draws++;
            if (inst > 1)
                m->inst_draws++;
            if (dist_m < m->dmin)
                m->dmin = dist_m;
            if (dist_m > m->dmax)
                m->dmax = dist_m;
            if (cls % BANDS >= 3 && area < 1e11f)
                m->far_area += area, m->far_tris_area += (double)tris;
        } else if (m) {
            m->shadow_tris += tris;
        } else if (cls >= 0) {
            g_full++;
        }
        ReleaseSRWLockExclusive(&g_mesh_lock);
    }
    if (ib)
        ID3D11Buffer_Release(ib);
    if (vb)
        ID3D11Buffer_Release(vb);
    if (lay)
        ID3D11InputLayout_Release(lay);
}

/* ---------------------------------------------------------------- writing */
static LONG64 far_tris(const Mesh* m) { return m->tris[3] + m->tris[4]; }

static int cmp_far(const void* a, const void* b)
{
    LONG64 x = far_tris(*(const Mesh* const*)a), y = far_tris(*(const Mesh* const*)b);
    return x < y ? 1 : x > y ? -1 : 0;
}

static const char* usage_name(UINT u)
{
    return u == D3D11_USAGE_IMMUTABLE ? "immutable" : u == D3D11_USAGE_DEFAULT ? "default"
                                                    : u == D3D11_USAGE_DYNAMIC ? "dynamic" : "staging";
}

/* bytes [off, off + n) of a buffer, read back through a staging copy on the immediate context */
static void* read_back(ID3D11DeviceContext* imm, ID3D11Buffer* src, UINT off, UINT n)
{
    D3D11_BUFFER_DESC sd;
    ID3D11Buffer_GetDesc(src, &sd);
    if (!n || off > sd.ByteWidth || n > sd.ByteWidth - off)
        return NULL;
    ID3D11Device* dev = NULL;
    ID3D11Buffer_GetDevice(src, &dev);
    if (!dev)
        return NULL;
    D3D11_BUFFER_DESC d = {n, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    ID3D11Buffer* st = NULL;
    void* out = NULL;
    if (SUCCEEDED(ID3D11Device_CreateBuffer(dev, &d, NULL, &st)) && st) {
        D3D11_BOX box = {off, 0, 0, off + n, 1, 1};
        ID3D11DeviceContext_CopySubresourceRegion(imm, (ID3D11Resource*)st, 0, 0, 0, 0, (ID3D11Resource*)src, 0, &box);
        D3D11_MAPPED_SUBRESOURCE mp;
        if (SUCCEEDED(ID3D11DeviceContext_Map(imm, (ID3D11Resource*)st, 0, D3D11_MAP_READ, 0, &mp))) {
            out = malloc(n);
            if (out)
                memcpy(out, mp.pData, n);
            ID3D11DeviceContext_Unmap(imm, (ID3D11Resource*)st, 0);
        }
        ID3D11Buffer_Release(st);
    }
    ID3D11Device_Release(dev);
    return out;
}

/* mesh_NNN.bin: a 64-byte header, the indices (uint32, from the lowest index used), the vertices
 * (raw, the position's vertex buffer, from that lowest index on) */
typedef struct {
    char magic[4];
    uint32_t version, index_count, vertex_count, vertex_stride, pos_format, pos_offset, first_vertex;
    float far_tris_per_frame, near_tris_per_frame, dist_min, dist_max, px_per_tri;
    uint32_t index_bits, reserved[2];
} MeshFileHeader;

static int dump_mesh(ID3D11DeviceContext* imm, const Mesh* m, const LayInfo* li, const char* path, int frames)
{
    if (li->pos_slot != 0 || !m->vb_stride) /* only positions in the first vertex buffer are recorded */
        return 0;
    UINT isz = m->ib_fmt == DXGI_FORMAT_R16_UINT ? 2 : 4;
    uint8_t* idx = (uint8_t*)read_back(imm, (ID3D11Buffer*)m->ib, m->ib_off + m->start * isz, m->count * isz);
    if (!idx)
        return 0;
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    uint32_t* ix = (uint32_t*)malloc(sizeof(uint32_t) * m->count);
    for (UINT i = 0; ix && i < m->count; i++) {
        uint32_t v = isz == 2 ? ((uint16_t*)idx)[i] : ((uint32_t*)idx)[i];
        ix[i] = v;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    free(idx);
    int ok = 0;
    if (ix && hi >= lo && hi - lo < 4000000) {
        UINT nv = hi - lo + 1;
        int64_t first = (int64_t)m->base + lo;
        uint8_t* vtx = first >= 0 ? (uint8_t*)read_back(imm, (ID3D11Buffer*)m->vb,
                                                        m->vb_off + (UINT)first * m->vb_stride, nv * m->vb_stride)
                                  : NULL;
        if (vtx) {
            for (UINT i = 0; i < m->count; i++)
                ix[i] -= lo;
            MeshFileHeader hd;
            memset(&hd, 0, sizeof(hd));
            memcpy(hd.magic, "MCM1", 4), hd.version = 1;
            hd.index_count = m->count, hd.vertex_count = nv, hd.vertex_stride = m->vb_stride;
            hd.pos_format = (uint32_t)li->pos_fmt, hd.pos_offset = (uint32_t)li->pos_off, hd.first_vertex = (uint32_t)first;
            hd.far_tris_per_frame = (float)far_tris(m) / frames;
            hd.near_tris_per_frame = (float)(m->tris[0] + m->tris[1] + m->tris[2]) / frames;
            hd.dist_min = m->dmin, hd.dist_max = m->dmax;
            hd.px_per_tri = m->far_tris_area > 0 ? (float)(m->far_area / m->far_tris_area) : 0;
            hd.index_bits = isz * 8;
            FILE* f = fopen(path, "wb");
            if (f) {
                fwrite(&hd, sizeof(hd), 1, f);
                fwrite(ix, sizeof(uint32_t), m->count, f);
                fwrite(vtx, m->vb_stride, nv, f);
                fclose(f);
                ok = 1;
            }
            free(vtx);
        }
    }
    free(ix);
    return ok;
}

static void census_write(ID3D11DeviceContext* imm)
{
    char dir[MAX_PATH];
    output_dir(dir, sizeof(dir), "meshes");
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\census.txt", dir);
    FILE* f = fopen(path, "w");
    int fr = g_rec_frames > 0 ? g_rec_frames : 1;
    Mesh** list = (Mesh**)malloc(sizeof(Mesh*) * MESH_N);
    int n = 0;
    for (int i = 0; list && i < MESH_N; i++)
        if (g_mesh[i].ib)
            list[n++] = &g_mesh[i];
    if (!f || !list) {
        if (f)
            fclose(f);
        free(list);
        LOGI("Mesh census: could not write %s", path);
        return;
    }
    qsort(list, (size_t)n, sizeof(Mesh*), cmp_far);
    LONG64 band[2 * BANDS] = {0}, far_all = 0, shadow_all = 0;
    for (int i = 0; i < n; i++) {
        for (int b = 0; b < 2 * BANDS; b++)
            band[b] += list[i]->tris[b];
        far_all += far_tris(list[i]);
        shadow_all += list[i]->shadow_tris;
    }
    fprintf(f, "MetaphorCPU %s mesh census: %d frames, %d meshes%s\n\n", MCPU_VERSION, fr, n,
            g_full ? " (table full: some draws not filed)" : "");
    fprintf(f, "Scene triangles per frame (thousands) by distance <15 / 15-30 / 30-50 / 50-100 / >100 m\n");
    fprintf(f, "  world      %7.1f %7.1f %7.1f %7.1f %7.1f\n", band[0] / 1e3 / fr, band[1] / 1e3 / fr,
            band[2] / 1e3 / fr, band[3] / 1e3 / fr, band[4] / 1e3 / fr);
    fprintf(f, "  characters %7.1f %7.1f %7.1f %7.1f %7.1f\n", band[5] / 1e3 / fr, band[6] / 1e3 / fr,
            band[7] / 1e3 / fr, band[8] / 1e3 / fr, band[9] / 1e3 / fr);
    fprintf(f, "  shadow-map triangles of these meshes %.1f k/frame; scene draws without readable buffers %.1f k/frame\n\n",
            shadow_all / 1e3 / fr, g_unknown_tris / 1e3 / fr);
    /* how few meshes make the triangles beyond 50 m */
    if (far_all > 0) {
        LONG64 acc = 0;
        int at50 = 0, at80 = 0, at95 = 0;
        for (int i = 0; i < n; i++) {
            acc += far_tris(list[i]);
            if (!at50 && acc * 2 >= far_all) at50 = i + 1;
            if (!at80 && acc * 5 >= far_all * 4) at80 = i + 1;
            if (!at95 && acc * 20 >= far_all * 19) at95 = i + 1;
        }
        fprintf(f, "Triangles beyond 50 m: %.1f k/frame, from %d meshes; 50%% of them in %d meshes, 80%% in %d, 95%% in %d\n",
                far_all / 1e3 / fr, n, at50, at80, at95);
        /* by buffer kind and vertex format */
        LONG64 by_usage[4] = {0}, by_init[2] = {0}, by_bits[2] = {0}, skinned = 0, inst = 0, slot_other = 0;
        LONG64 by_pos[8] = {0};
        int pos_fmts[8] = {0}, npf = 0;
        for (int i = 0; i < n; i++) {
            const Mesh* m = list[i];
            LONG64 t = far_tris(m);
            if (!t)
                continue;
            BufInfo bi = buf_info(m->ib), bv = buf_info(m->vb);
            LayInfo li = lay_info(m->lay);
            by_usage[bi.usage & 3] += t;
            by_init[bi.init && bv.init] += t;
            by_bits[m->ib_fmt == DXGI_FORMAT_R16_UINT ? 0 : 1] += t;
            if (li.skinned) skinned += t;
            if (m->inst_draws) inst += t;
            if (li.pos_slot != 0) slot_other += t;
            int k = 0;
            while (k < npf && pos_fmts[k] != li.pos_fmt) k++;
            if (k == npf && npf < 8) pos_fmts[npf++] = li.pos_fmt;
            if (k < 8) by_pos[k] += t;
        }
        fprintf(f, "  index buffers: immutable %.0f%%, default %.0f%%, dynamic %.0f%% | created with their data (index and vertex buffer) %.0f%%\n",
                100.0 * by_usage[D3D11_USAGE_IMMUTABLE] / far_all, 100.0 * by_usage[D3D11_USAGE_DEFAULT] / far_all,
                100.0 * by_usage[D3D11_USAGE_DYNAMIC] / far_all, 100.0 * by_init[1] / far_all);
        fprintf(f, "  16-bit indices %.0f%%, 32-bit %.0f%% | skinned layouts %.0f%% | instanced draws %.0f%% | position not in buffer 0 %.0f%%\n",
                100.0 * by_bits[0] / far_all, 100.0 * by_bits[1] / far_all, 100.0 * skinned / far_all,
                100.0 * inst / far_all, 100.0 * slot_other / far_all);
        fprintf(f, "  position formats (DXGI):");
        for (int k = 0; k < npf; k++)
            fprintf(f, " %d = %.0f%%", pos_fmts[k], 100.0 * by_pos[k] / far_all);
        fprintf(f, "\n\n");
    }
    fprintf(f, "Top meshes by triangles beyond 50 m (per frame). px/tri = on-screen pixels per triangle at 720p beyond 50 m.\n");
    fprintf(f, "rank  far_k  near_k  shad_k draws/f  indices  dist_m      px/tri  ib            vb                         layout\n");
    for (int i = 0; i < n && i < 150; i++) {
        const Mesh* m = list[i];
        if (!far_tris(m))
            break;
        BufInfo bi = buf_info(m->ib), bv = buf_info(m->vb);
        LayInfo li = lay_info(m->lay);
        fprintf(f, "%4d %6.1f %7.1f %7.1f %7.2f %8u %5.0f-%-5.0f %7.2f  %2d/%-9s%s  %3u/%-9s%s%s  %s\n", i, far_tris(m) / 1e3 / fr,
                (m->tris[0] + m->tris[1] + m->tris[2]) / 1e3 / fr, m->shadow_tris / 1e3 / fr, (double)m->draws / fr,
                m->count, m->dmin, m->dmax, m->far_tris_area > 0 ? m->far_area / m->far_tris_area : 0,
                m->ib_fmt == DXGI_FORMAT_R16_UINT ? 16 : 32, usage_name(bi.usage), bi.init ? "+data" : "",
                m->vb_stride, usage_name(bv.usage), bv.init ? "+data" : "", li.skinned ? " skinned" : "",
                li.desc[0] ? li.desc : "?");
    }
    /* the top 40 as files */
    int dumped = 0;
    for (int i = 0; imm && i < n && i < 40 && far_tris(list[i]); i++) {
        LayInfo li = lay_info(list[i]->lay);
        char mp[MAX_PATH];
        snprintf(mp, sizeof(mp), "%s\\mesh_%03d.bin", dir, i);
        dumped += dump_mesh(imm, list[i], &li, mp, fr);
    }
    fprintf(f, "\n%d of the top 40 meshes written as mesh_NNN.bin\n", dumped);
    fclose(f);
    free(list);
    LOGI("Mesh census: %d frames, %d meshes, %.0f k triangles/frame beyond 50 m; %d meshes saved -> %s", fr, n,
         far_all / 1e3 / fr, dumped, dir);
}

static void census_clear(void)
{
    for (int i = 0; i < MESH_N; i++) {
        Mesh* m = &g_mesh[i];
        if (m->ib) {
            ID3D11Buffer_Release((ID3D11Buffer*)m->ib);
            ID3D11Buffer_Release((ID3D11Buffer*)m->vb);
            if (m->lay)
                ID3D11InputLayout_Release((ID3D11InputLayout*)m->lay);
        }
    }
    memset(g_mesh, 0, sizeof(Mesh) * MESH_N);
    g_nmesh = 0, g_full = 0, g_unknown_tris = 0;
}

/* starts a recording of n frames (F9, or a test) */
void census_start(int frames)
{
    if (g_rec)
        return;
    if (!g_mesh)
        g_mesh = (Mesh*)calloc(MESH_N, sizeof(Mesh));
    if (!g_mesh)
        return;
    AcquireSRWLockExclusive(&g_mesh_lock);
    census_clear();
    ReleaseSRWLockExclusive(&g_mesh_lock);
    g_rec_frames = 0, g_rec_left = frames;
    InterlockedExchange(&g_rec, 1);
    LOGI("Mesh census: recording %d frames", frames);
}

/* on the presenting thread, after Present; imm = the device's immediate context (for the read-back) */
void census_frame_end(void* imm)
{
    if (!g_rec) {
        if (GetAsyncKeyState(VK_F9) & 1)
            census_start(300);
        return;
    }
    g_rec_frames++;
    if (--g_rec_left > 0)
        return;
    InterlockedExchange(&g_rec, 0);
    AcquireSRWLockExclusive(&g_mesh_lock); /* draws still being recorded on other threads finish first */
    census_write((ID3D11DeviceContext*)imm);
    census_clear();
    ReleaseSRWLockExclusive(&g_mesh_lock);
}
