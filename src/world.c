// The world file (world.h): opening and checking it, chunk lookup, and saving the last played date.
// Runs at startup, on chunk loads and at exit: plain C is fine here.
#include <string.h>
#include <fileioc.h>
#include <sys/rtc.h>

#include "engine.h"
#include "world.h"

#define CRC_START  __builtin_offsetof(world_header_t, name)   // the checksum covers the rest

#ifndef __INTELLISENSE__
_Static_assert(sizeof(world_header_t) == 48, "world_header_t must match utils/make_world.py");
_Static_assert(__builtin_offsetof(world_header_t, crc) == 5, "world_header_t layout");
_Static_assert(CRC_START == 9, "the checksum must cover everything after it");
_Static_assert(__builtin_offsetof(world_header_t, size_x) == 32, "world_header_t layout");
_Static_assert(__builtin_offsetof(world_header_t, spawn_yaw) == 38, "world_header_t layout");
_Static_assert(__builtin_offsetof(world_header_t, ground) == 39, "world_header_t layout");
_Static_assert(sizeof(world_chunk_t) == 3 && sizeof(world_voxel_t) == 4, "world record layout");
#endif

const world_header_t *world = NULL;
static uint8_t  handle = 0;
static uint16_t world_size;            // bytes


// ---- CRC-32 ----
// Two steps per byte through a 16 entry table (64 bytes), instead of a 256 entry one (1 KB in the
// program plus 1 KB of bss for the split copy). Slower, but it only runs in worldOpen and worldClose
static const uint32_t crc_nib[16] = {
    0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL, 0x76DC4190UL, 0x6B6B51F4UL,
    0x4DB26158UL, 0x5005713CUL, 0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
    0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL,
};

uint32_t crc32(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ crc_nib[crc & 15];
        crc = (crc >> 4) ^ crc_nib[crc & 15];
    }
    return ~crc;
}


// ---- opening ----

// every chunk inside the file, after the index, and every voxel in range. The engine relies on it:
// chunk-local x and z keep the painter order and the grid indices valid, y bounds the sort keys
static uint8_t worldCheck(const uint8_t *data) {
    const world_header_t *h = (const world_header_t *)data;
    if (h->size_x == 0 || h->size_z == 0) {
        return WORLD_BAD_DATA;
    }
    if (h->ground != WORLD_NO_GROUND && h->ground >= MAT_COUNT) {
        return WORLD_BAD_DATA;
    }
    const uint24_t chunks = (uint24_t)h->size_x * h->size_z;
    const uint24_t index_end = sizeof(world_header_t) + chunks * sizeof(world_chunk_t);
    if (index_end > world_size) {
        return WORLD_BAD_DATA;
    }
    const world_chunk_t *c = (const world_chunk_t *)(data + sizeof(world_header_t));
    for (uint24_t i = 0; i < chunks; i++, c++) {
        if (c->offset < index_end) {
            return WORLD_BAD_DATA;
        }
        if ((uint24_t)c->offset + c->count * sizeof(world_voxel_t) > world_size) {
            return WORLD_BAD_DATA;
        }
        const world_voxel_t *v = (const world_voxel_t *)(data + c->offset);
        for (uint8_t k = c->count; k != 0; k--, v++) {
            // ifs, not a combined test: see the compiler bug in CLAUDE.md
            if (v->xz >= CHUNK_SIZE * CHUNK_SIZE) {
                return WORLD_BAD_DATA;
            }
            if (v->faces > (FACE_XP | FACE_XN | FACE_ZP | FACE_ZN | FACE_TOP | FACE_BOT)) {
                return WORLD_BAD_DATA;
            }
            if (v->y < WORLD_Y_MIN) {
                return WORLD_BAD_DATA;
            }
            if (v->y > WORLD_Y_MAX) {
                return WORLD_BAD_DATA;
            }
            if (v->mat >= MAT_COUNT) {
                return WORLD_BAD_DATA;
            }
        }
    }
    return WORLD_OK;
}

uint8_t worldOpen(const char *appvar) {
    handle = ti_Open(appvar, "r");
    if (!handle) {
        return WORLD_MISSING;
    }
    world_size = ti_GetSize(handle);
    const uint8_t *data = ti_GetDataPtr(handle);
    const world_header_t *h = (const world_header_t *)data;
    uint8_t err = WORLD_OK;
    if (world_size < sizeof(world_header_t)) {
        err = WORLD_BAD_HEADER;
    } else if (memcmp(h->magic, "VXWD", 4) != 0) {
        err = WORLD_BAD_HEADER;
    } else if (h->version != WORLD_VERSION) {
        err = WORLD_BAD_HEADER;
    } else {
        if (crc32(0, data + CRC_START, world_size - CRC_START) != h->crc) {
            err = WORLD_BAD_CRC;
        } else {
            err = worldCheck(data);
        }
    }
    if (err != WORLD_OK) {
        ti_Close(handle);
        handle = 0;
        return err;
    }
    world = h;
    return WORLD_OK;
}

const char *worldError(uint8_t err) {
    switch (err) {
        // at most 26 characters: one line of the home screen
        case WORLD_MISSING:    return "No world AppVar " WORLD_APPVAR;
        case WORLD_BAD_HEADER: return "Not a world, or other ver.";
        case WORLD_BAD_CRC:    return "World corrupted (checksum)";
        case WORLD_BAD_DATA:   return "World has bad data";
        default:               return "World OK";
    }
}

const world_voxel_t *worldChunk(int24_t cx, int24_t cz, uint8_t *count) {
    // ifs: see the compiler bug in CLAUDE.md
    *count = 0;
    if (cx < 0 || cz < 0) {
        return NULL;
    }
    if (cx >= world->size_x || cz >= world->size_z) {
        return NULL;
    }
    const uint8_t *data = (const uint8_t *)world;
    const world_chunk_t *c = (const world_chunk_t *)(data + sizeof(world_header_t)) +
                             (cz * world->size_x + cx);
    *count = c->count;
    return (const world_voxel_t *)(data + c->offset);
}


// ---- closing ----

uint8_t worldClose(void) {
    if (!handle) {
        return 0;
    }
    uint8_t saved = 0;
    const int archived = ti_IsArchived(handle);
    world = NULL;   // the AppVar may move from here on
    if (!archived || ti_SetArchiveStatus(false, handle)) {
        uint8_t *data = ti_GetDataPtr(handle);
        world_header_t *h = (world_header_t *)data;
        uint8_t day, month, seconds, minutes, hours;
        uint16_t year;
        boot_GetDate(&day, &month, &year);
        boot_GetTime(&seconds, &minutes, &hours);
        h->last_played.year = year;
        h->last_played.month = month;
        h->last_played.day = day;
        h->last_played.hour = hours;
        h->last_played.minute = minutes;
        h->last_played.second = seconds;
        h->crc = crc32(0, data + CRC_START, world_size - CRC_START);
        saved = 1;
        if (archived) {
            ti_SetArchiveStatus(true, handle);
        }
    }
    ti_Close(handle);
    handle = 0;
    return saved;
}
