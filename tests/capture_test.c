/* Frame capture: the PNG writer and the target conversion, on synthetic data. Reads the written
 * files back (chunk CRCs, zlib stored blocks, Adler-32) and checks pixel values. */
#include "../src/capture.c"

/* effects.c (not linked here): CompareShots=10 asks it for the near characters' pixel shaders */
int fx_near_char_ps(uint64_t* out, int max) { (void)out, (void)max; return 0; }

static int fails;
#define EXPECT(c, what)                                                                                         \
    do {                                                                                                        \
        printf("  [%s] %s\n", (c) ? "PASS" : "FAIL", what);                                                     \
        if (!(c))                                                                                               \
            fails++;                                                                                            \
    } while (0)

static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* decodes the PNG this writer makes: RGB, 8 bit, filter 0, stored blocks */
static uint8_t* read_png(const char* path, int* w, int* h, int* crc_ok)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    static uint8_t buf[8 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    static uint8_t z[8 << 20];
    size_t zn = 0, o = 8;
    *crc_ok = memcmp(buf, "\x89PNG\r\n\x1a\n", 8) == 0;
    while (o + 12 <= n) {
        uint32_t len = get32(buf + o);
        const uint8_t* type = buf + o + 4;
        uint32_t c = crc_upd(0xFFFFFFFFu, type, 4 + len) ^ 0xFFFFFFFFu;
        if (c != get32(buf + o + 8 + len))
            *crc_ok = 0;
        if (!memcmp(type, "IHDR", 4))
            *w = (int)get32(buf + o + 8), *h = (int)get32(buf + o + 12);
        if (!memcmp(type, "IDAT", 4))
            memcpy(z + zn, buf + o + 8, len), zn += len;
        o += 12 + len;
    }
    size_t raw = (size_t)*h * (1 + 3 * (size_t)*w);
    uint8_t* out = (uint8_t*)malloc(raw);
    size_t p = 2, d = 0;
    uint32_t a = 1, b = 0;
    for (;;) {
        int last = z[p] & 1;
        size_t len = z[p + 1] | (size_t)z[p + 2] << 8;
        memcpy(out + d, z + p + 5, len);
        for (size_t i = 0; i < len; i++)
            a = (a + z[p + 5 + i]) % 65521u, b = (b + a) % 65521u;
        d += len, p += 5 + len;
        if (last)
            break;
    }
    if (d != raw || get32(z + p) != ((b << 16) | a))
        *crc_ok = 0;
    return out;
}

static uint16_t f2h(float f) /* enough for the test values */
{
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t s = (x >> 16) & 0x8000, e = (x >> 23) & 0xFF, m = x & 0x7FFFFF;
    if (e == 0)
        return (uint16_t)s;
    return (uint16_t)(s | ((e - 112) << 10) | (m >> 13));
}

int main(void)
{
    crc_init();
    char dir[MAX_PATH];
    GetTempPathA(sizeof(dir), dir);
    char path[MAX_PATH], stats[400];
    /* RGBA16F 64x32: red = x / 8 (0..7.9, HDR), green = 1, blue = 0 */
    static uint16_t px[32][64][4];
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 64; x++)
            px[y][x][0] = f2h(x / 8.0f), px[y][x][1] = f2h(1.0f), px[y][x][2] = 0, px[y][x][3] = f2h(0.5f);
    D3D11_MAPPED_SUBRESOURCE m = {px, 64 * 8, 0};
    snprintf(path, sizeof(path), "%smcpu_capture_test_f16.png", dir);
    int ok = convert(&m, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_TYPELESS, 64, 32, path, stats, sizeof(stats), 960);
    printf("        RGBA16F stats: %s\n", stats);
    EXPECT(ok, "RGBA16F target written");
    int w = 0, h = 0, crc = 0;
    uint8_t* img = read_png(path, &w, &h, &crc);
    EXPECT(img && w == 64 && h == 32 && crc, "PNG reads back (size, chunk CRCs, Adler-32)");
    if (img) {
        /* x = 8: red 1.0 -> (1 / 2) ^ (1 / 2.2) = 0.73 -> 186; green 1.0 -> 186; blue 0 */
        const uint8_t* p8 = img + 5 * (1 + 3 * 64) + 1 + 3 * 8;
        printf("        pixel (8, 5): %u %u %u\n", p8[0], p8[1], p8[2]);
        EXPECT(p8[0] >= 184 && p8[0] <= 188 && p8[1] >= 184 && p8[1] <= 188 && p8[2] == 0, "tone-mapped HDR value");
        EXPECT(img[5 * (1 + 3 * 64)] == 0, "filter byte 0");
        free(img);
    }
    EXPECT(strstr(stats, "R 0..7.88") && strstr(stats, "A 0.5..0.5"), "value ranges of every channel");
    /* RG16F signed (a velocity-like target): -2..2 maps to 0..255 around 128 */
    static uint16_t v[16][16][2];
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            v[y][x][0] = f2h(x < 8 ? -2.0f : 2.0f), v[y][x][1] = 0;
    D3D11_MAPPED_SUBRESOURCE m2 = {v, 16 * 4, 0};
    snprintf(path, sizeof(path), "%smcpu_capture_test_rg16f.png", dir);
    ok = convert(&m2, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, 16, 16, path, stats, sizeof(stats), 960);
    img = read_png(path, &w, &h, &crc);
    EXPECT(ok && img && crc, "RG16F target written");
    if (img) {
        const uint8_t* a = img + 1, *b = img + 1 + 3 * 12;
        printf("        signed pixels: %u / %u, green %u\n", a[0], b[0], a[1]);
        EXPECT(a[0] == 0 && b[0] == 255 && a[1] >= 127 && a[1] <= 128, "signed values shown around mid grey");
        free(img);
    }
    /* RGBA8 1200 wide: shrunk to 600 */
    static uint8_t big[4][1200][4];
    memset(big, 200, sizeof(big));
    D3D11_MAPPED_SUBRESOURCE m3 = {big, 1200 * 4, 0};
    snprintf(path, sizeof(path), "%smcpu_capture_test_rgba8.png", dir);
    ok = convert(&m3, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_TYPELESS, 1200, 4, path, stats, sizeof(stats), 960);
    img = read_png(path, &w, &h, &crc);
    EXPECT(ok && img && w == 600 && h == 2 && img[1] == 200, "wide RGBA8 target halved, values kept");
    free(img);
    /* R11G11B10F: 1.0 / 2.0 / 0.5 */
    uint32_t r11 = (15u << 6) | ((16u << 6) << 11) | ((14u << 5) << 22), one[4] = {r11, r11, r11, r11};
    D3D11_MAPPED_SUBRESOURCE m4 = {one, 8, 0};
    snprintf(path, sizeof(path), "%smcpu_capture_test_r11.png", dir);
    ok = convert(&m4, DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT, 2, 2, path, stats, sizeof(stats), 960);
    printf("        R11G11B10F stats: %s\n", stats);
    EXPECT(ok && strstr(stats, "R 1..1") && strstr(stats, "G 2..2") && strstr(stats, "B 0.5..0.5"), "R11G11B10F decoded");
    printf("\n%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
