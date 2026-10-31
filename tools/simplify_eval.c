/* Offline: how much simpler the game's far meshes can get without a visible change.
 * Reads the mesh_NNN.bin files of a mesh census (meshcensus.c) and simplifies each with
 * meshoptimizer at an absolute error of a fraction of a pixel at the distance where that version
 * would be used: versions for draws whose nearest point (the census distance: camera to the
 * bounding sphere) is at least 25, 50, 100 or 200 m away. Prints the triangles left, with and without the mesh border locked
 * (a border that moves can open cracks against the next mesh), the far triangles per frame they
 * would save, and how long simplifying takes. Positions are taken as model units = cm.
 *
 * usage: simplify_eval <census folder> [--px 0.5] [--cm-per-px-per-m 0.13] [--obj N]
 *   --px              allowed error in pixels (default 0.5)
 *   --cm-per-px-per-m size of one pixel 1 m away (default 0.13: 900 lines, ~60 deg vertical view)
 *   --obj N           also write mesh N and its simpler version as .obj files into the folder */
#include "../third_party/meshoptimizer/src/meshoptimizer.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef struct {
    char magic[4];
    uint32_t version, index_count, vertex_count, vertex_stride, pos_format, pos_offset, first_vertex;
    float far_tris_per_frame, near_tris_per_frame, dist_min, dist_max, px_per_tri;
    uint32_t index_bits, reserved[2];
} MeshFileHeader;

static float half_to_float(uint16_t h)
{
    uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF, x;
    if (e == 0)
        x = s; /* denormals: rare in positions, taken as 0 */
    else if (e == 31)
        x = s | 0x7F800000u | (m << 13);
    else
        x = s | ((e + 112) << 23) | (m << 13);
    float f;
    memcpy(&f, &x, 4);
    return f;
}

/* float3 positions from the raw vertices; 0 if the format is not a plain float / half one */
static int positions(const MeshFileHeader* h, const uint8_t* v, float* out)
{
    for (uint32_t i = 0; i < h->vertex_count; i++) {
        const uint8_t* p = v + (size_t)i * h->vertex_stride + h->pos_offset;
        switch (h->pos_format) {
        case 2: /* R32G32B32A32_FLOAT */
        case 6: /* R32G32B32_FLOAT */
            memcpy(out + 3 * i, p, 12);
            break;
        case 10: /* R16G16B16A16_FLOAT */
            for (int k = 0; k < 3; k++)
                out[3 * i + k] = half_to_float(((const uint16_t*)p)[k]);
            break;
        default:
            return 0;
        }
    }
    return 1;
}

static void write_obj(const char* path, const float* pos, uint32_t nv, const unsigned* idx, size_t ni)
{
    FILE* f = fopen(path, "w");
    if (!f)
        return;
    for (uint32_t i = 0; i < nv; i++)
        fprintf(f, "v %g %g %g\n", pos[3 * i], pos[3 * i + 1], pos[3 * i + 2]);
    for (size_t i = 0; i + 2 < ni; i += 3)
        fprintf(f, "f %u %u %u\n", idx[i] + 1, idx[i + 1] + 1, idx[i + 2] + 1);
    fclose(f);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: simplify_eval <census folder> [--px 0.5] [--cm-per-px-per-m 0.13] [--obj N]\n");
        return 1;
    }
    const char* dir = argv[1];
    float px = 0.5f, cm_px_m = 0.13f;
    int obj = -1;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--px"))
            px = (float)atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--cm-per-px-per-m"))
            cm_px_m = (float)atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--obj"))
            obj = atoi(argv[i + 1]);
    }
    /* versions for draws whose nearest point is at least 25 / 50 / 100 / 200 m away; each within
     * the allowed error at that distance */
    static const float lvl_m[4] = {25, 50, 100, 200};
    printf("Allowed error: %.2f px = %.1f / %.1f / %.1f / %.1f cm for versions used from 25 / 50 / 100 / 200 m\n", px,
           px * cm_px_m * 25, px * cm_px_m * 50, px * cm_px_m * 100, px * cm_px_m * 200);
    printf("A mesh uses the version for its nearest distance during the census (camera to its bounding sphere).\n\n");
    printf("mesh  far_k/f  dist_m       size_m      tris  px/tri | version  locked   free  seams | ms\n");
    double far_all = 0, far_after_locked = 0, far_after_free = 0, far_after_seams = 0, ms_all = 0, tris_all = 0;
    int files = 0;
    LARGE_INTEGER qf, q0, q1;
    QueryPerformanceFrequency(&qf);
    for (int n = 0; n < 1000; n++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s\\mesh_%03d.bin", dir, n);
        FILE* f = fopen(path, "rb");
        if (!f)
            continue;
        MeshFileHeader h;
        if (fread(&h, sizeof(h), 1, f) != 1 || memcmp(h.magic, "MCM1", 4)) {
            fclose(f);
            continue;
        }
        unsigned* idx = (unsigned*)malloc(sizeof(unsigned) * h.index_count);
        uint8_t* vtx = (uint8_t*)malloc((size_t)h.vertex_count * h.vertex_stride);
        float* pos = (float*)malloc(sizeof(float) * 3 * h.vertex_count);
        int ok = idx && vtx && pos && fread(idx, sizeof(unsigned), h.index_count, f) == h.index_count &&
                 fread(vtx, h.vertex_stride, h.vertex_count, f) == h.vertex_count;
        fclose(f);
        files++;
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        if (ok && positions(&h, vtx, pos)) {
            for (uint32_t i = 0; i < h.vertex_count; i++)
                for (int k = 0; k < 3; k++) {
                    if (pos[3 * i + k] < lo[k]) lo[k] = pos[3 * i + k];
                    if (pos[3 * i + k] > hi[k]) hi[k] = pos[3 * i + k];
                }
            float size = 0;
            for (int k = 0; k < 3; k++) {
                float e = hi[k] - lo[k];
                if (e > size)
                    size = e;
            }
            float near_m = h.dist_min; /* the census measures to the bounding sphere already */
            int lv = -1;
            for (int k = 0; k < 4; k++)
                if (near_m >= lvl_m[k])
                    lv = k;
            double t = h.index_count / 3.0, kl = 1, kf = 1, kp = 1, ms = 0;
            if (lv >= 0) {
                unsigned* out = (unsigned*)malloc(sizeof(unsigned) * h.index_count);
                float err = px * cm_px_m * lvl_m[lv], e;
                QueryPerformanceCounter(&q0);
                size_t rl = meshopt_simplify(out, idx, h.index_count, pos, h.vertex_count, 12, 0, err,
                                             meshopt_SimplifyErrorAbsolute | meshopt_SimplifyLockBorder, &e);
                QueryPerformanceCounter(&q1);
                ms = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart;
                size_t rf = meshopt_simplify(out, idx, h.index_count, pos, h.vertex_count, 12, 0, err,
                                             meshopt_SimplifyErrorAbsolute, &e);
                if (n == obj) {
                    char op[1024];
                    snprintf(op, sizeof(op), "%s\\mesh_%03d.obj", dir, n);
                    write_obj(op, pos, h.vertex_count, idx, h.index_count);
                    snprintf(op, sizeof(op), "%s\\mesh_%03d_simple.obj", dir, n);
                    write_obj(op, pos, h.vertex_count, out, rf);
                }
                /* also across texture seams (border locked): normals / UVs may shift at the seams */
                size_t rp = meshopt_simplify(out, idx, h.index_count, pos, h.vertex_count, 12, 0, err,
                                             meshopt_SimplifyErrorAbsolute | meshopt_SimplifyLockBorder |
                                                 meshopt_SimplifyPermissive,
                                             &e);
                kl = rl / 3.0 / t, kf = rf / 3.0 / t, kp = rp / 3.0 / t;
                free(out);
                ms_all += ms, tris_all += t;
            }
            far_all += h.far_tris_per_frame;
            far_after_locked += h.far_tris_per_frame * kl;
            far_after_free += h.far_tris_per_frame * kf;
            far_after_seams += h.far_tris_per_frame * kp;
            char ver[16];
            if (lv >= 0)
                snprintf(ver, sizeof(ver), "%3.0f m", lvl_m[lv]);
            else
                snprintf(ver, sizeof(ver), "none");
            printf("%4d %8.1f %5.0f-%-5.0f %7.0f %9.0f %7.2f | %-7s %6.0f%% %6.0f%% %5.0f%% | %.0f\n", n,
                   h.far_tris_per_frame / 1e3, h.dist_min, h.dist_max, size / 100.0f, t, h.px_per_tri, ver,
                   100.0 * kl, 100.0 * kf, 100.0 * kp, ms);
        } else {
            printf("%4d %8.1f  position format %u not readable (stride %u, offset %u)\n", n,
                   h.far_tris_per_frame / 1e3, h.pos_format, h.vertex_stride, h.pos_offset);
        }
        free(idx);
        free(vtx);
        free(pos);
    }
    if (!files) {
        printf("no mesh_NNN.bin in %s\n", dir);
        return 1;
    }
    if (far_all > 0)
        printf("\nFar triangles of these meshes: %.1f k/frame -> %.1f k with the border locked (%.0f%%), %.1f k free (%.0f%%)\n",
               far_all / 1e3, far_after_locked / 1e3, 100.0 * far_after_locked / far_all, far_after_free / 1e3,
               100.0 * far_after_free / far_all);
    if (far_all > 0)
        printf("Also across texture seams (border locked): %.1f k (%.0f%%)\n", far_after_seams / 1e3,
               100.0 * far_after_seams / far_all);
    if (tris_all > 0)
        printf("Simplifying took %.0f ms for %.2f M triangles (%.0f ms per million, one core of this PC)\n", ms_all,
               tris_all / 1e6, ms_all / (tris_all / 1e6));
    return 0;
}
