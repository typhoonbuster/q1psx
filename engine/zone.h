/*
 * q1psx — memory allocation
 *
 * Ported from id-Software/Quake WinQuake/zone.h, kept close to the original so
 * the BSP / model / QuakeC loaders port with minimal edits.
 *
 * Differences from stock Quake (see docs/porting-notes.md §3):
 *   - Cache_??? (LRU-evictable memory) is NOT implemented yet. It exists mostly
 *     to swap sounds/models under memory pressure; on the PS1 sound lives in
 *     SPU RAM and level data stays resident, and mid-game eviction would mean
 *     disk hitches (we target constant 60fps). Call sites will be shimmed to
 *     Hunk_Alloc when phases 5/8 need them.
 *   - Hunk/Zone otherwise behave exactly as in the original.
 *
 * -------- layout of the single contiguous block passed to Memory_Init --------
 *
 *   low  ->  [ zone block ][ low hunk (permanent level data) ->      ...
 *                                       ... <- (temp) high hunk ]  <- high
 *
 * Hunk allocations are 16-byte aligned. Memory is only reclaimed by resetting
 * a mark (Hunk_FreeToLowMark / Hunk_FreeToHighMark).
 */

#pragma once

void  Memory_Init(void *buf, int size);

/* --- Zone: small dynamic allocations (strings, small structs) --- */
void  Z_Free(void *ptr);
void *Z_Malloc(int size);            /* returns zero-filled memory */
void *Z_TagMalloc(int size, int tag);
void  Z_CheckHeap(void);
int   Z_FreeMemory(void);

/* --- Hunk: big permanent allocations, stack discipline from both ends --- */
void *Hunk_Alloc(int size);          /* returns zero-filled memory */
void *Hunk_AllocName(int size, const char *name);
void *Hunk_HighAllocName(int size, const char *name);

int   Hunk_LowMark(void);
void  Hunk_FreeToLowMark(int mark);
int   Hunk_HighMark(void);
void  Hunk_FreeToHighMark(int mark);

void *Hunk_TempAlloc(int size);

void  Hunk_Check(void);

/* --- q1psx diagnostics (not in stock Quake) --- */
int   Hunk_LowUsed(void);
int   Hunk_HighUsed(void);
int   Hunk_TotalSize(void);
void  Mem_Report(void);              /* prints hunk/zone usage to serial */
