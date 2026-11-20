// The world file: an AppVar holding a header, a chunk index and the voxels of every chunk.
// utils/make_world.py writes it and documents the byte layout; the structs below are the same.
//
// The world is split into chunk columns of CHUNK_SIZE x CHUNK_SIZE cells (x and z), of any height,
// with at most CHUNK_MAX voxels each. The engine keeps the 7 x 7 chunks around the camera in RAM
// (chunks.c) and reads the rest from the file as the camera moves. An archived AppVar is read in
// place, straight from flash.
//
// The ground plane: a world may say that everything at y >= 0 is solid ground of one material.
// The engine draws it as the background below the horizon, so it costs nothing. The file holds no
// voxels of the ground material at y >= 0; voxels of other materials at y = 0 (paths, patches)
// are drawn over it, top face only. Every voxel's exposed faces are stored in the file
// (make_world.py computes them, counting the ground as solid), so loading a chunk is a copy.
#ifndef WORLD_H
#define WORLD_H

#include <stdint.h>
#include <stddef.h>

#define WORLD_APPVAR     "WORLD"   // the AppVar the engine opens
#define WORLD_VERSION    2
#define WORLD_NAME_LEN   16        // name bytes, NUL terminated
#define CHUNK_SIZE       8         // cells per chunk along x and z
#define CHUNK_MAX        255       // voxels per chunk
#define WORLD_Y_MIN      -96       // y cells a voxel may occupy (y grows downward, ground at 0)
#define WORLD_Y_MAX      95
#define WORLD_NO_GROUND  0xff      // world_header_t.ground: no ground plane

// errors of worldOpen()
#define WORLD_OK         0
#define WORLD_MISSING    1         // no such AppVar
#define WORLD_BAD_HEADER 2         // not a world file, or another format version
#define WORLD_BAD_CRC    3         // the checksum does not match: the file is corrupted
#define WORLD_BAD_DATA   4         // the checksum matches, but the contents are out of range

typedef struct {
    uint16_t year;                 // 0: never played
    uint8_t  month, day, hour, minute, second;
} world_date_t;

typedef struct {
    char         magic[4];         // "VXWD"
    uint8_t      version;          // WORLD_VERSION
    uint32_t     crc;              // CRC-32 (zlib's) of every byte after this field
    char         name[WORLD_NAME_LEN];
    world_date_t last_played;
    uint8_t      size_x, size_z;   // in chunks; chunk (cx, cz) covers cells 8 cx .. 8 cx + 7
    int16_t      spawn_x, spawn_z; // spawn cell
    uint8_t      spawn_yaw;        // 256 steps per turn, 0 looks along +z
    uint8_t      ground;           // material of the ground plane (y >= 0), or WORLD_NO_GROUND
    uint8_t      reserved[8];
} world_header_t;

typedef struct {
    uint16_t offset;               // of the chunk's voxels, from the start of the file
    uint8_t  count;
} world_chunk_t;

typedef struct {
    uint8_t xz;                    // x + 8 z, x and z within the chunk (0 .. CHUNK_SIZE - 1)
    int8_t  y;                     // cell, WORLD_Y_MIN .. WORLD_Y_MAX
    uint8_t mat;                   // index into materials[]
    uint8_t faces;                 // exposed FACE_* (engine.h)
} world_voxel_t;

// the open world's header (in the AppVar: read only while it is archived). NULL until worldOpen()
extern const world_header_t *world;

// Opens the AppVar, checks the header and the checksum, and checks that every chunk lies inside
// the file and every voxel is in range (so the engine needs no checks later). WORLD_OK or an error
uint8_t worldOpen(const char *appvar);
// a short message for a worldOpen() error
const char *worldError(uint8_t err);
// Opens WORLD_APPVAR before graphx starts. On an error it says so on the home screen, waits for a
// key and returns 0
uint8_t worldStart(void);
// the voxels of chunk (cx, cz) and their number in *count. NULL and 0 outside the world
const world_voxel_t *worldChunk(int24_t cx, int24_t cz, uint8_t *count);
// Stores the current date and time as last played, updates the checksum and closes the world.
// An archived world is moved to RAM for that and archived again, which needs free RAM the size of
// the world (otherwise the date is left as it was). Call it after gfx_End(): archiving may run a
// garbage collection, which shows its own screen. Returns 0 if the date could not be saved
uint8_t worldClose(void);

// CRC-32 as zlib computes it, continuing from crc (0 to start)
uint32_t crc32(uint32_t crc, const void *data, size_t len);

#endif
