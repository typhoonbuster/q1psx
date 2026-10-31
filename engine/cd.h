/*
 * q1psx — CD-ROM file loading.
 *
 * Files are read whole, in double-speed mode, straight into the level arena.
 * Names are ISO9660 paths as built by iso/iso.xml, e.g. "\\MAPS\\E1M1_G2.WLD;1":
 * a file in the root or in a directory of the root. A lookup reads its
 * directory into the level arena's top (released at once: the arena must
 * have room for it, 24 KB for \MAPS on a disc with every map), never into
 * the SDK's heap (psxcd's CdSearchFile and CdOpenDir: see cd.c).
 */

#ifndef Q1PSX_CD_H
#define Q1PSX_CD_H

#include <stdint.h>

#define CD_SECTOR 2048           /* a data sector's bytes */

void  cd_init(void);

/* Called about once per frame while a read is in flight, with the bytes read
   so far (whole sectors). It may draw and flip a frame (a loading bar). */
typedef void (*cd_poll_fn)(uint32_t bytes_done);

/* Load a whole file into the level arena. Returns NULL if it is missing or
   the read fails; *size receives the exact byte size. With `poll`, the read
   runs in the background and `poll` is called until it is done; NULL
   blocks. */
void *cd_load(const char *name, uint32_t *size, cd_poll_fn poll);

/* The drive, shared by the map's loads, the streamed assets, the SPU
   music and the world's chunks (docs/porting-notes.md §57). psxcd's
   command writes race the drive's interrupt: its handler switches the
   register bank and resets the parameter FIFO, so an interrupt arriving
   while a command's parameters are written (a read's Pause answering
   late, a sector on its way) sends the command without them (the drive's
   error 20h: after a lost Setloc the read reads on from wherever the
   drive was) and the parameters into the interrupt enable register (the
   drive gone quiet). So every command waits for the drive to have been
   idle CD_SETTLE vblanks:
     cd_ready     the drive takes a command (no read, no blocking command,
                  idle long enough): the reads started between frames wait
                  for it, frame after frame;
     cd_seek      a Setloc for a read: the drive settled first (waited for,
                  the reads started at once), its answer checked; refused:
                  the drive's interrupts enabled again, sent again;
     cd_read_start the read itself after the seek (CdReadRetry): every
                  read of the disc starts here, with who asks it and for
                  what (the bench's journal, docs/porting-notes.md §83);
     cd_poll      a read's progress (CdReadSync(1)): the sectors still to
                  come, the drive's busy time kept;
     cd_read_sync the end of a read waited for (CdReadSync(0)), likewise;
     cd_read_cut  a read in flight broken off (CdReadBreak; its owner
                  then waits for it with cd_read_sync, and pauses the drive);
     cd_read_now  a whole read at once (seek, read, wait). */
enum { CDW_DIR, CDW_FILE, CDW_ZONE, CDW_MODEL, CDW_SOUND, CDW_MUSIC, CDW_N };   /* who reads: a directory
                  or a header, a whole file, a world chunk, a streamed model, a streamed sound's
                  piece, the music's refill */
int   cd_ready(void);
int   cd_seek(int32_t lba);
int   cd_read_start(int32_t lba, uint32_t sectors, void *dst, int who, int tag);
int   cd_poll(void);
int   cd_read_sync(void);
void  cd_read_cut(void);
int   cd_read_now(int32_t lba, uint32_t sectors, void *dst, int who, int tag);
void  cd_busy(void);              /* (a command just sent outside these: the drive settles from now) */

#ifdef Q1PSX_BENCH
/* The bench's journal and drive model (docs/porting-notes.md §83).
     cd_log    1: a `cd` line per read when it ends: who, its tag (the zone,
               the asset, the music's chunk), its first sector, its sectors,
               where the head was (the last read's end), the vblanks it was
               started and ended at, the seek the model gives it (µs), the
               polls the model held it, its end ('.', 'e' an error, 'x' cut);
     cd_model  1: a read ends no sooner than a real drive would end it:
               DuckStation's seek model (from measures of the mechanism) plus
               the sectors at double speed, from the seek command; the drive
               is busy until then (cd_ready);
     cd_log_mark names the phase that starts (a map's load, its walk): a
               `cd sum` line totals the phase before it. */
extern int cd_log, cd_model;
void  cd_log_mark(const char *name);
#else
#define cd_log_mark(name) ((void) 0)
#endif

/* Is the file on the disc? (No read of the file; *size may be NULL.) Each
   call reads its directory again (a seek and a read: ~80 ms on the drive). */
int   cd_exists(const char *name, uint32_t *size);

/* The file's first sector (CdIntToPos's numbering) and its size, as
   cd_exists; either pointer may be NULL. */
int   cd_find(const char *name, uint32_t *lba, uint32_t *size);

/* The files of a directory ("\\LVL\\E1M1"), one call of `fn` each with the
   ISO name ("E1M1.WLD;1"), its first sector (CdIntToPos's numbering)
   and its size: one directory read for a whole listing (world_scan_maps
   asked cd_exists 128 times: 10 s on the drive).
   Returns the count, 0 if the directory is missing. */
typedef void (*cd_list_fn)(const char *name, uint32_t lba, uint32_t size);
int   cd_list_dir(const char *path, cd_list_fn fn);

/* The directories of a directory ("\\LVL"), one call of `fn` each with
   the name ("E1M1"): the disc's levels, one directory each under \LVL
   (docs/porting-notes.md §84). */
typedef void (*cd_dir_fn)(const char *name);
int   cd_list_dirs(const char *path, cd_dir_fn fn);

/* A level's directory read once, at its load, and kept: cd_find answers
   from it for that directory's files without a read (the level's files,
   its copy of the music's track). Returns its files' count, 0 if it is
   missing (then cd_find reads directories as before). */
int   cd_cache_dir(const char *path);

#endif
