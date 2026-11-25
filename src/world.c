// The world file (world.h): opening and checking it, chunk lookup, and saving the last played date.
// Runs at startup, on chunk loads and at exit: plain C is fine here.
#include <string.h>
#include <fileioc.h>
#include <sys/rtc.h>
#include <sys/timers.h>

#include "display.h"
#include "engine.h"
#include "world.h"

#define CRC_BLOCK  512    // bytes of CRC between progress bar updates
#define CRC_START  __builtin_offsetof(world_header_t, name)   // the checksum covers the rest

#ifndef __INTELLISENSE__
_Static_assert(sizeof(world_header_t) == 48, "world_header_t must match utils/make_world.py");
_Static_assert(__builtin_offsetof(world_header_t, crc) == 5, "world_header_t layout");
_Static_assert(CRC_START == 9, "the checksum must cover everything after it");
_Static_assert(__builtin_offsetof(world_header_t, size_x) == 32, "world_header_t layout");
_Static_assert(__builtin_offsetof(world_header_t, spawn_yaw) == 38, "world_header_t layout");
_Static_assert(__builtin_offsetof(world_header_t, ground) == 39, "world_header_t layout");
_Static_assert(sizeof(world_chunk_t) == 4 && sizeof(world_voxel_t) == 4, "world record layout");
#endif

const world_header_t *world = NULL;
static uint8_t  handle = 0;
static uint16_t world_size;            // bytes

world_entry_t world_list[WORLD_LIST_MAX];
uint8_t world_count = 0;
uint8_t world_current = WORLD_NONE;


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

// every chunk inside the file, after the index, and every record in range. The engine relies on it:
// chunk-local x and z keep the painter order and the grid indices valid, y bounds the sort keys, a
// run is at most RUN_MAX long (project.S has room for that many) and of its chunk's orientation (or
// the painter order would be wrong for it)
static uint8_t worldCheck(const uint8_t *data, uint24_t size) {
    const world_header_t *h = (const world_header_t *)data;
    if (h->size_x == 0 || h->size_z == 0) {
        return WORLD_BAD_DATA;
    }
    if (h->ground != WORLD_NO_GROUND && h->ground >= MAT_COUNT) {
        return WORLD_BAD_DATA;
    }
    const uint24_t chunks = (uint24_t)h->size_x * h->size_z;
    const uint24_t index_end = sizeof(world_header_t) + chunks * sizeof(world_chunk_t);
    if (index_end > size) {
        return WORLD_BAD_DATA;
    }
    const world_chunk_t *c = (const world_chunk_t *)(data + sizeof(world_header_t));
    for (uint24_t i = 0; i < chunks; i++, c++) {
        if (c->offset < index_end) {
            return WORLD_BAD_DATA;
        }
        if ((uint24_t)c->offset + c->count * sizeof(world_voxel_t) > size) {
            return WORLD_BAD_DATA;
        }
        if (c->runs > WORLD_RUNS_Z) {
            return WORLD_BAD_DATA;
        }
        uint8_t run_faces = 0;
        if (c->runs == WORLD_RUNS_X) {
            run_faces = FACE_XP | FACE_XN;
        }
        if (c->runs == WORLD_RUNS_Z) {
            run_faces = FACE_ZP | FACE_ZN;
        }
        const world_voxel_t *v = (const world_voxel_t *)(data + c->offset);
        uint8_t in_runs = 0;            // the runs come after the voxels
        for (uint8_t k = c->count; k != 0; k--, v++) {
            // ifs, not a combined test: see the compiler bug in CLAUDE.md
            if (v->xz >= CHUNK_SIZE * CHUNK_SIZE) {
                return WORLD_BAD_DATA;
            }
            if (v->y < WORLD_Y_MIN) {
                return WORLD_BAD_DATA;
            }
            if (v->y > WORLD_Y_MAX) {
                return WORLD_BAD_DATA;
            }
            if (v->faces & FACE_RUN_REC) {
                // a run: one face of the orientation, length 1 .. RUN_MAX, inside the chunk
                in_runs = 1;
                const uint8_t face = v->faces & ~FACE_RUN_REC & 0xff;
                if (face == 0) {
                    return WORLD_BAD_DATA;
                }
                if (face & (face - 1)) {
                    return WORLD_BAD_DATA;
                }
                if ((face & run_faces) == 0) {
                    return WORLD_BAD_DATA;
                }
                const uint8_t len = (v->mat >> 4) + 1;
                if (len > RUN_MAX) {
                    return WORLD_BAD_DATA;
                }
                uint8_t along = v->xz & 7;          // a z run: along x
                if (run_faces & (FACE_XP | FACE_XN)) {
                    along = v->xz >> 3;             // an x run: along z
                }
                if (along + len > CHUNK_SIZE) {
                    return WORLD_BAD_DATA;
                }
            } else {
                if (in_runs) {
                    return WORLD_BAD_DATA;
                }
                if (v->faces > (FACE_XP | FACE_XN | FACE_ZP | FACE_ZN | FACE_TOP | FACE_BOT | FACE_RUN)) {
                    return WORLD_BAD_DATA;
                }
                if (v->mat >= MAT_COUNT) {
                    return WORLD_BAD_DATA;
                }
            }
        }
    }
    return WORLD_OK;
}

// a before b: played later (never played, year 0, sorts last)
static uint8_t playedLater(const world_date_t *a, const world_date_t *b) {
    if (a->year != b->year) {
        return a->year > b->year;
    }
    const uint8_t *x = &a->month, *y = &b->month;     // month, day, hour, minute, second
    for (uint8_t i = 0; i < 5; i++) {
        if (x[i] != y[i]) {
            return x[i] > y[i];
        }
    }
    return 0;
}

void worldScan(void) {
    world_count = 0;
    world_current = WORLD_NONE;
    void *vat = NULL;
    const char *var;
    while (world_count < WORLD_LIST_MAX && (var = ti_Detect(&vat, WORLD_MAGIC)) != NULL) {
        world_entry_t e;
        memset(&e, 0, sizeof e);
        strncpy(e.appvar, var, sizeof e.appvar - 1);
        const uint8_t h = ti_Open(e.appvar, "r");
        if (!h) {
            continue;
        }
        if (ti_GetSize(h) >= sizeof(world_header_t)) {
            const world_header_t *hd = ti_GetDataPtr(h);
            memcpy(e.name, hd->name, WORLD_NAME_LEN - 1);
            e.last_played = hd->last_played;
        }
        ti_Close(h);
        // insertion, most recently played first
        uint8_t i = world_count++;
        while (i > 0 && playedLater(&e.last_played, &world_list[i - 1].last_played)) {
            world_list[i] = world_list[i - 1];
            i--;
        }
        world_list[i] = e;
    }
}

uint8_t worldOpen(const char *appvar) {
    const uint8_t h_new = ti_Open(appvar, "r");
    if (!h_new) {
        return WORLD_MISSING;
    }
    const uint24_t size = ti_GetSize(h_new);
    const uint8_t *data = ti_GetDataPtr(h_new);
    const world_header_t *h = (const world_header_t *)data;
    uint8_t err = WORLD_OK;
    if (size < sizeof(world_header_t)) {
        err = WORLD_BAD_HEADER;
    } else if (memcmp(h->magic, WORLD_MAGIC, 4) != 0) {
        err = WORLD_BAD_HEADER;
    } else if (h->version != WORLD_VERSION) {
        err = WORLD_BAD_HEADER;
    } else {
        // in blocks, for the progress bar (crc32 chains: it inverts on entry and exit)
        const uint24_t len = size - CRC_START;
        uint32_t crc = 0;
        for (uint24_t done = 0; done < len; ) {
            const uint24_t n = len - done < CRC_BLOCK ? len - done : CRC_BLOCK;
            crc = crc32(crc, data + CRC_START + done, n);
            done += n;
            displayProgress(done * LOADING_CRC / len);      // < 2^24: len < 2^16
        }
        if (crc != h->crc) {
            err = WORLD_BAD_CRC;
        } else {
            err = worldCheck(data, size);
            displayProgress(LOADING_CHECK);
        }
    }
    if (err != WORLD_OK) {
        ti_Close(h_new);            // the open world stays
        return err;
    }
    worldClose(0);
    handle = h_new;
    world_size = size;
    world = h;
    return WORLD_OK;
}

uint8_t worldSelect(uint8_t i) {
    const uint8_t err = worldOpen(world_list[i].appvar);
    if (err == WORLD_OK) {
        world_current = i;
    }
    return err;
}

const char *worldError(uint8_t err) {
    switch (err) {
        // at most 26 characters: one line of the home screen
        case WORLD_MISSING:    return "World AppVar not found";
        case WORLD_BAD_HEADER: return "Not a world, or other ver.";
        case WORLD_BAD_CRC:    return "World corrupted (checksum)";
        case WORLD_BAD_DATA:   return "World has bad data";
        default:               return "World OK";
    }
}

uint8_t worldStart(void) {
    worldScan();
    if (world_count == 0) {
        displayError("No worlds found.", "Send a world AppVar (.8xv)");
        return 0;
    }
    uint8_t first_err = WORLD_OK;
    for (uint8_t i = 0; i < world_count; i++) {
        const uint8_t err = worldSelect(i);
        if (err == WORLD_OK) {
            return 1;
        }
        if (first_err == WORLD_OK) {
            first_err = err;
        }
    }
    displayError(worldError(first_err), world_list[0].appvar);
    return 0;
}

const world_voxel_t *worldChunk(int24_t cx, int24_t cz, uint8_t *count, uint8_t *runs) {
    // ifs: see the compiler bug in CLAUDE.md
    *count = 0;
    *runs = WORLD_RUNS_NONE;
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
    *runs = c->runs;
    return (const world_voxel_t *)(data + c->offset);
}


// ---- closing ----

uint8_t worldClose(uint8_t save) {
    if (!handle) {
        return 0;
    }
    uint8_t saved = 0;
    const int archived = ti_IsArchived(handle);
    world = NULL;   // the AppVar may move from here on
    world_current = WORLD_NONE;
    if (save && (!archived || ti_SetArchiveStatus(false, handle))) {
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
