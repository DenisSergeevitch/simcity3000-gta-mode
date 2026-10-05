#include "world.h"
#include "camera.h"

bool world_read(WorldInfo *w) {
    w->ok = false;
    Camera c; if (!cam_read(&c)) return false;
    int32_t dims[2]; uint32_t cols;
    if (!mem_read(c.view + 0x14, dims, 8) || !mem_read(c.view + 0x24, &cols, 4) || !cols) return false;
    if (dims[0] <= 0 || dims[0] > 256 || dims[1] <= 0 || dims[1] > 256) return false;
    w->mapw = dims[0]; w->maph = dims[1]; w->cols = cols; w->ok = true;
    return true;
}

bool world_tile(const WorldInfo *w, int tx, int tz, uint8_t rec[20]) {
    if (!w->ok || tx < 0 || tz < 0 || tx >= w->mapw || tz >= w->maph) return false;
    uint32_t col; if (!mem_read(w->cols + 4 * tx, &col, 4) || !col) return false;
    return mem_read(col + 20 * tz, rec, 20);
}

bool world_inside(const WorldInfo *w, float x, float z) {
    return w->ok && x >= 0 && z >= 0 && x < w->mapw * 256.0f && z < w->maph * 256.0f;
}

float world_ground(const WorldInfo *w, float x, float z) {
    uint8_t r[20];
    if (!world_tile(w, (int)(x / 256), (int)(z / 256), r)) return 0;
    return r[0xb] * 256.0f;
}
