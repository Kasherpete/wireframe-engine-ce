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
//
// Runs: besides its voxels a chunk may hold runs, records that each stand for up to RUN_MAX
// coplanar side faces in a row (chunks.h explains how they are drawn and kept in order), after its
// voxels. A chunk's runs all have its orientation: faces +x / -x (WORLD_RUNS_X) or +z / -z
// (WORLD_RUNS_Z).
#ifndef WORLD_H
#define WORLD_H

#include <stdint.h>
#include <stddef.h>

#define WORLD_MAGIC      "VXWD"    // the first bytes of a world AppVar (any name): worldScan
#define WORLD_LIST_MAX   16        // worlds worldScan lists (the rest are left out)
#define WORLD_VERSION    3
#define WORLD_NAME_LEN   16        // name bytes, NUL terminated
#define CHUNK_SIZE       8         // cells per chunk along x and z
#define CHUNK_MAX        255       // records (voxels and runs) per chunk
#define RUN_MAX          8         // cells per run (a chunk row)
#define WORLD_Y_MIN      -96       // y cells a voxel may occupy (y grows downward, ground at 0)
#define WORLD_Y_MAX      95
#define WORLD_NO_GROUND  0xff      // world_header_t.ground: no ground plane

// errors of worldOpen()
#define WORLD_OK         0
#define WORLD_MISSING    1         // no such AppVar
#define WORLD_BAD_HEADER 2         // not a world file, or another format version
#define WORLD_BAD_CRC    3         // the checksum does not match: the file is corrupted
#define WORLD_BAD_DATA   4         // the checksum matches, but the contents are out of range
#define WORLD_NO_MEMORY  5         // worldRename, worldCreate: not enough free RAM (or no free name)

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
    // where the player was at the last "Save & Quit" (reserved bytes in older files: 0, none)
    uint8_t      saved;            // 1: the fields below hold a position
    int16_t      saved_x, saved_z; // cell
    int8_t       saved_feet;       // the cell of the feet (y grows downward)
    uint8_t      saved_yaw;        // 256 steps per turn
    uint8_t      reserved;
} world_header_t;

// world_chunk_t.runs
#define WORLD_RUNS_NONE  0
#define WORLD_RUNS_X     1         // runs of +x / -x faces, along z
#define WORLD_RUNS_Z     2         // runs of +z / -z faces, along x

typedef struct {
    uint16_t offset;               // of the chunk's records, from the start of the file
    uint8_t  count;                // voxels and runs
    uint8_t  runs;                 // WORLD_RUNS_*: the orientation of its runs
} world_chunk_t;

// A voxel, or a run when faces has FACE_RUN_REC (engine.h): then mat is material + 16 (length - 1)
// and faces FACE_RUN_REC + its one face, and the run covers that face of the cells (x, y, z + i)
// (x faces) or (x + i, y, z) (z faces) for i = 0 .. length - 1, all in the chunk
typedef struct {
    uint8_t xz;                    // x + 8 z, x and z within the chunk (0 .. CHUNK_SIZE - 1)
    int8_t  y;                     // cell, WORLD_Y_MIN .. WORLD_Y_MAX
    uint8_t mat;                   // index into materials[] (a run: + 16 (length - 1))
    uint8_t faces;                 // exposed FACE_*, FACE_RUN (engine.h); a run: FACE_RUN_REC + face
} world_voxel_t;

// the open world's header (in the AppVar: read only while it is archived). NULL until worldOpen()
extern const world_header_t *world;

// a world AppVar found by worldScan: from its header, unchecked (worldOpen checks it)
typedef struct {
    char         appvar[9];        // the AppVar name, NUL terminated
    char         name[WORLD_NAME_LEN];   // the world name, NUL terminated (any bytes: sanitize)
    world_date_t last_played;
} world_entry_t;

extern world_entry_t world_list[WORLD_LIST_MAX];
extern uint8_t world_count;        // entries in world_list
extern uint8_t world_current;      // world_list index of the open world, WORLD_NONE if none
#define WORLD_NONE       0xff

// Lists the AppVars (RAM or archive) whose data starts with WORLD_MAGIC, most recently played
// first, and finds the open world among them again (world_current)
void worldScan(void);
// the world_list index of the AppVar, WORLD_NONE if it is not listed
uint8_t worldFind(const char *appvar);
// Opens the AppVar, checks the header and the checksum, and checks that every chunk lies inside
// the file and every voxel is in range (so the engine needs no checks later). Then it replaces the
// open world (closed unsaved). On an error the open world stays as it was. WORLD_OK or an error
uint8_t worldOpen(const char *appvar);
// worldOpen of world_list[i], setting world_current
uint8_t worldSelect(uint8_t i);
// a short message for a worldOpen() error
const char *worldError(uint8_t err);
// Before graphx starts: worldScan, then opens the most recently played world that passes the
// checks. WORLD_OK, or the first world's error (WORLD_MISSING if there is none: world_count 0);
// then no world is open (world NULL) and the title says so
uint8_t worldStart(void);
// the records of chunk (cx, cz), their number in *count and their run orientation in *runs (as
// world_chunk_t). NULL, 0 and WORLD_RUNS_NONE outside the world
const world_voxel_t *worldChunk(int24_t cx, int24_t cz, uint8_t *count, uint8_t *runs);
// Saving (worldClose(1)) and renaming move an archived world to RAM, which needs its size in free
// RAM (plus WORLD_RAM_MARGIN for the OS). The game itself takes most of the RAM while it runs, so
// a large world may not fit: then they do nothing rather than let the OS stop the program with
// ERR:MEMORY. worldCanSave: whether the open world can be saved now; *need gets the free RAM that
// takes (0 if the world is in RAM already) and *avail the free RAM (either may be NULL)
#define WORLD_RAM_MARGIN 256
uint8_t worldCanSave(uint24_t *need, uint24_t *avail);
// "Save & Quit": the position worldClose(1) stores in the header (playerSave in player.c)
void worldSetPlayer(int16_t x, int16_t z, int8_t feet, uint8_t yaw);
// Closes the world. With save, first stores the current date and time as last played (and the
// position of worldSetPlayer, if it was called) and updates the checksum (only for a world that was played, not one only seen behind the title). An archived world is moved to RAM for that and archived again, which needs free RAM the size of
// the world (otherwise nothing is saved and it returns 0). Call it after gfx_End(): archiving may run a
// garbage collection, which shows its own screen. Returns 0 if the date could not be saved
uint8_t worldClose(uint8_t save);

// While graphx runs (title screen): renames world_list[i] (name cut to WORLD_NAME_LEN - 1
// characters) and updates its checksum. An archived world is moved to RAM for it (needs free RAM
// the size of the world) and archived again by worldArchivePending. Then the list is scanned again
// (indices may change: worldFind). WORLD_OK, WORLD_NO_MEMORY or WORLD_BAD_HEADER
uint8_t worldRename(uint8_t i, const char *name);
// Creates an empty world (8 x 8 chunks on a green ground plane, spawn in the middle) in a new RAM
// AppVar WORLDn (the first free name, into appvar, 9 bytes), archived by worldArchivePending; then
// scans the list again. WORLD_OK or WORLD_NO_MEMORY
uint8_t worldCreate(const char *name, char *appvar);
// After gfx_End and worldClose: archives the AppVars worldRename and worldCreate left in RAM (may
// show the OS garbage collection screen; one that does not fit stays in RAM)
void worldArchivePending(void);

// CRC-32 as zlib computes it, continuing from crc (0 to start)
uint32_t crc32(uint32_t crc, const void *data, size_t len);

#endif
