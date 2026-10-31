/* DXBC shader bytecode: reading the resource definitions (RDEF) and rewriting constant-buffer reads.
 *
 * Used by [Quality] RimLight (effects.c): the game's toon material shaders add a rim light,
 * matEdgeColor x the light colour x matEdgeColor.w, where the surface turns away from the camera. A copy
 * of such a shader in which every read of matEdgeColor reads 0 adds no rim. The copy is made once per
 * shader at creation; the container's chunk sizes and checksum are rebuilt. Anything the parser does not
 * fully understand leaves the shader alone (NULL).
 * Used by [Quality] InnerOutlines too: in the outline shaders only the multiplication that makes the opacity of
 * the lines over the character's own body reads outlineColor.w, and there it reads 0. */
#include "mcpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* the chunk with this FourCC: its data and size, NULL if absent */
static const uint8_t* dxbc_chunk(const uint8_t* code, size_t len, const char* cc, uint32_t* size)
{
    if (len < 32 || memcmp(code, "DXBC", 4))
        return NULL;
    uint32_t n = rd32(code + 28);
    for (uint32_t i = 0; i < n && 32 + 4 * (size_t)i + 4 <= len; i++) {
        uint32_t off = rd32(code + 32 + 4 * i);
        if ((size_t)off + 8 > len || memcmp(code + off, cc, 4))
            continue;
        uint32_t sz = rd32(code + off + 4);
        if ((size_t)off + 8 + sz > len)
            return NULL;
        *size = sz;
        return code + off + 8;
    }
    return NULL;
}

static const char* rdef_str(const uint8_t* r, uint32_t size, uint32_t off)
{
    if (off >= size || !memchr(r + off, 0, size - off))
        return "";
    return (const char*)r + off;
}

int dxbc_cbuffer_var(const uint8_t* code, size_t len, const char* cbuf, const char* var, int* slot, int* offset)
{
    uint32_t size;
    const uint8_t* r = dxbc_chunk(code, len, "RDEF", &size);
    if (!r || size < 28)
        return 0;
    uint32_t ncb = rd32(r), cboff = rd32(r + 4), nbind = rd32(r + 8), bindoff = rd32(r + 12);
    int major = r[17]; /* version at 16: minor, major, program type (2 bytes); shader model 5 variables are 40 bytes */
    uint32_t vstride = major >= 5 ? 40 : 24;
    *slot = -1, *offset = -1;
    for (uint32_t i = 0; i < nbind; i++) { /* the bind point of the constant buffer (input type 0) */
        const uint8_t* b = r + bindoff + 32 * i;
        if (bindoff + 32 * (i + 1) > size)
            return 0;
        if (rd32(b + 4) == 0 && !strcmp(rdef_str(r, size, rd32(b)), cbuf))
            *slot = (int)rd32(b + 20);
    }
    for (uint32_t i = 0; i < ncb; i++) {
        const uint8_t* c = r + cboff + 24 * i;
        if (cboff + 24 * (i + 1) > size)
            return 0;
        if (strcmp(rdef_str(r, size, rd32(c)), cbuf))
            continue;
        uint32_t nv = rd32(c + 4), voff = rd32(c + 8);
        for (uint32_t k = 0; k < nv; k++) {
            const uint8_t* v = r + voff + vstride * k;
            if (voff + vstride * (k + 1) > size)
                return 0;
            if (!strcmp(rdef_str(r, size, rd32(v)), var))
                *offset = (int)rd32(v + 4);
        }
    }
    return *slot >= 0 && *offset >= 0;
}

/* the resources a shader binds, from RDEF: "name:kind@slot" separated by spaces (kinds: cb, tex, samp, uav, sbuf,
 * rwsbuf, raw, rwraw, ...). Returns the number listed. */
int dxbc_bind_names(const uint8_t* code, size_t len, char* out, size_t cap)
{
    static const char* const kind[] = {"cb", "tbuf", "tex", "samp", "uav", "sbuf", "rwsbuf", "raw", "rwraw", "append",
                                       "consume", "rwsbufc"};
    uint32_t size;
    size_t at = 0;
    int n = 0;
    if (cap)
        out[0] = 0;
    const uint8_t* r = dxbc_chunk(code, len, "RDEF", &size);
    if (!r || size < 28 || !cap)
        return 0;
    uint32_t nbind = rd32(r + 8), bindoff = rd32(r + 12);
    for (uint32_t i = 0; i < nbind && at + 1 < cap; i++) {
        const uint8_t* b = r + bindoff + 32 * i;
        if (bindoff + 32 * (i + 1) > size)
            break;
        uint32_t t = rd32(b + 4);
        int w = snprintf(out + at, cap - at, "%s%s:%s@%u", n ? " " : "", rdef_str(r, size, rd32(b)),
                         t < sizeof(kind) / sizeof(kind[0]) ? kind[t] : "?", rd32(b + 20));
        if (w < 0 || (size_t)w >= cap - at) {
            out[at] = 0;
            break;
        }
        at += (size_t)w, n++;
    }
    return n;
}

int dxbc_texture_slot(const uint8_t* code, size_t len, const char* name)
{
    uint32_t size;
    const uint8_t* r = dxbc_chunk(code, len, "RDEF", &size);
    if (!r || size < 28)
        return -1;
    uint32_t nbind = rd32(r + 8), bindoff = rd32(r + 12);
    for (uint32_t i = 0; i < nbind; i++) {
        const uint8_t* b = r + bindoff + 32 * i;
        if (bindoff + 32 * (i + 1) > size)
            return -1;
        if (rd32(b + 4) == 2 /* D3D_SIT_TEXTURE */ && !strcmp(rdef_str(r, size, rd32(b)), name))
            return (int)rd32(b + 20);
    }
    return -1;
}

/* ---------------------------------------------------------------- SHEX / SHDR rewriting */
enum { OT_IMM32 = 4, OT_IMM64 = 5, OT_CB = 8 };

typedef struct Out {
    uint32_t* w;
    size_t n, cap;
} Out;

static int put(Out* o, uint32_t v)
{
    if (o->n == o->cap) {
        size_t c = o->cap ? o->cap * 2 : 1024;
        uint32_t* w = (uint32_t*)realloc(o->w, c * 4);
        if (!w)
            return 0;
        o->w = w, o->cap = c;
    }
    o->w[o->n++] = v;
    return 1;
}

/* what a read becomes: the register, the component read (-1 any) and the value */
typedef struct Want {
    int slot, reg, comp;
    uint32_t value;
} Want;

/* the one component an operand reads: select-1, or a swizzle of a single component; -1 otherwise */
static int one_comp(uint32_t t)
{
    int ncomp = (int)(t & 3), mode = (int)(t >> 2 & 3);
    if (ncomp != 2)
        return -1;
    if (mode == 2)
        return (int)(t >> 4 & 3);
    int s = (int)(t >> 4 & 0xFF);
    return mode == 1 && (s & 3) == (s >> 2 & 3) && (s & 3) == (s >> 4 & 3) && (s & 3) == (s >> 6 & 3) ? (s & 3) : -1;
}

/* Copies one operand from w[*p] (end e) to o, replacing a read of cb<slot>[reg] by the wanted value (NULL: none
 * replaced). Returns 0 on a form it does not understand. */
static int operand(const uint32_t* w, size_t* p, size_t e, Out* o, const Want* want, int* replaced)
{
    if (*p >= e)
        return 0;
    size_t start = *p;
    uint32_t t = w[(*p)++];
    int ncomp = (int)(t & 3), mode = (int)(t >> 2 & 3), type = (int)(t >> 12 & 0xFF), dim = (int)(t >> 20 & 3);
    int rep[3] = {(int)(t >> 22 & 7), (int)(t >> 25 & 7), (int)(t >> 28 & 7)};
    uint32_t ext = t;
    while (ext >> 31) { /* extended operand tokens (modifiers) */
        if (*p >= e)
            return 0;
        ext = w[(*p)++];
    }
    if (want && type == OT_CB && dim == 2 && rep[0] == 0 && rep[1] == 0 && *p + 2 <= e && (int)w[*p] == want->slot &&
        (int)w[*p + 1] == want->reg && (!(t >> 31) || !want->value) && (want->comp < 0 || one_comp(t) == want->comp)) {
        *p += 2;
        (*replaced)++;
        uint32_t v = want->value;
        if (ncomp == 1 || (ncomp == 2 && mode == 2)) /* a scalar or one selected component: a scalar */
            return put(o, 0x00004001) && put(o, v);
        return put(o, 0x00004002) && put(o, v) && put(o, v) && put(o, v) && put(o, v);
    }
    if (type == OT_IMM32)
        *p += ncomp == 1 ? 1 : ncomp == 2 ? 4 : 0;
    else if (type == OT_IMM64)
        *p += ncomp == 1 ? 2 : ncomp == 2 ? 8 : 0;
    for (int i = 0; i < dim; i++) {
        switch (rep[i]) {
        case 0: *p += 1; break; /* immediate 32 */
        case 1: *p += 2; break; /* immediate 64 */
        case 2: { Out tmp = {0}; int r2 = 0; int ok = operand(w, p, e, &tmp, NULL, &r2); free(tmp.w); if (!ok) return 0; break; }
        case 3: { *p += 1; Out tmp = {0}; int r2 = 0; int ok = operand(w, p, e, &tmp, NULL, &r2); free(tmp.w); if (!ok) return 0; break; }
        case 4: { *p += 2; Out tmp = {0}; int r2 = 0; int ok = operand(w, p, e, &tmp, NULL, &r2); free(tmp.w); if (!ok) return 0; break; }
        default: return 0;
        }
    }
    if (*p > e)
        return 0;
    for (size_t k = start; k < *p; k++) /* unchanged: copied as it was (relative indices included) */
        if (!put(o, w[k]))
            return 0;
    return 1;
}

/* declarations (dcl_*: tokens that are not all operands) and the hull-shader phase markers: copied unchanged */
static int is_decl(int op)
{
    return (op >= 0x58 && op <= 0x6A) || (op >= 0x71 && op <= 0x74) || (op >= 0x8F && op <= 0xA2) || op == 0xCE;
}

uint8_t* dxbc_zero_cb_reads(const uint8_t* code, size_t len, int slot, int reg, size_t* out_len, int* replaced)
{
    return dxbc_patch_cb_reads(code, len, slot, reg, -1, -1, 0.0f, out_len, replaced);
}

uint8_t* dxbc_patch_cb_reads(const uint8_t* code, size_t len, int slot, int reg, int opcode, int comp, float value,
                             size_t* out_len, int* replaced)
{
    *replaced = 0;
    Want want = {slot, reg, comp, 0};
    memcpy(&want.value, &value, 4);
    uint32_t ssz;
    const char* cc = "SHEX";
    const uint8_t* sh = dxbc_chunk(code, len, cc, &ssz);
    if (!sh) {
        cc = "SHDR";
        sh = dxbc_chunk(code, len, cc, &ssz);
    }
    if (!sh || ssz < 8 || (ssz & 3))
        return NULL;
    const uint32_t* w = (const uint32_t*)sh;
    size_t nw = ssz / 4;
    if (w[1] != nw)
        return NULL;
    Out o = {0};
    int ok = put(&o, w[0]) && put(&o, 0); /* the length is written at the end */
    size_t p = 2;
    while (ok && p < nw) {
        uint32_t t = w[p];
        int op = (int)(t & 0x7FF);
        size_t ilen = op == 0x35 ? (p + 1 < nw ? w[p + 1] : 0) : (t >> 24 & 0x7F); /* customdata: length follows */
        if (!ilen || p + ilen > nw) {
            ok = 0;
            break;
        }
        if (op == 0x35 || is_decl(op)) { /* copied as they are */
            for (size_t k = 0; k < ilen && ok; k++)
                ok = put(&o, w[p + k]);
            p += ilen;
            continue;
        }
        size_t at = o.n, q = p, e = p + ilen;
        ok = put(&o, w[q++]);
        uint32_t x = t;
        while (ok && (x >> 31)) { /* extended opcode tokens */
            if (q >= e) {
                ok = 0;
                break;
            }
            x = w[q++];
            ok = put(&o, x);
        }
        const Want* wn = (opcode < 0 || op == opcode) ? &want : NULL;
        while (ok && q < e)
            ok = operand(w, &q, e, &o, wn, replaced);
        if (!ok || q != e)
            break;
        size_t nl = o.n - at;
        if (nl > 127) {
            ok = 0;
            break;
        }
        o.w[at] = (o.w[at] & ~(0x7Fu << 24)) | (uint32_t)nl << 24;
        p = e;
    }
    if (!ok || p != nw || !*replaced) {
        free(o.w);
        return NULL;
    }
    o.w[1] = (uint32_t)o.n;
    /* the container again, with the new chunk */
    uint32_t nchunks = rd32(code + 28);
    size_t newsz = 32 + 4 * (size_t)nchunks;
    for (uint32_t i = 0; i < nchunks; i++) {
        uint32_t off = rd32(code + 32 + 4 * i), sz = rd32(code + off + 4);
        newsz += 8 + (memcmp(code + off, cc, 4) ? sz : (uint32_t)o.n * 4);
    }
    uint8_t* b = (uint8_t*)malloc(newsz);
    if (!b) {
        free(o.w);
        return NULL;
    }
    memcpy(b, code, 32);
    size_t at = 32 + 4 * (size_t)nchunks;
    for (uint32_t i = 0; i < nchunks; i++) {
        uint32_t off = rd32(code + 32 + 4 * i), sz = rd32(code + off + 4);
        uint32_t at32 = (uint32_t)at;
        memcpy(b + 32 + 4 * i, &at32, 4);
        memcpy(b + at, code + off, 4);
        if (memcmp(code + off, cc, 4)) {
            memcpy(b + at + 4, &sz, 4);
            memcpy(b + at + 8, code + off + 8, sz);
            at += 8 + sz;
        } else {
            uint32_t nsz = (uint32_t)o.n * 4;
            memcpy(b + at + 4, &nsz, 4);
            memcpy(b + at + 8, o.w, nsz);
            at += 8 + nsz;
        }
    }
    uint32_t total = (uint32_t)newsz;
    memcpy(b + 24, &total, 4);
    uint32_t sum[4];
    fx_dxbc_checksum(b, newsz, sum);
    memcpy(b + 4, sum, 16);
    free(o.w);
    *out_len = newsz;
    return b;
}
