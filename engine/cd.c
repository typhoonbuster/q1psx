#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <psxcd.h>

#include "cd.h"
#include "mem.h"

#define SECTOR 2048

void cd_init(void)
{
	CdInit();
}

int cd_exists(const char *name, uint32_t *size)
{
	CdlFILE file;
	if (!CdSearchFile(&file, name))
		return 0;
	if (size)
		*size = (uint32_t) file.size;
	return 1;
}

void *cd_load(const char *name, uint32_t *size, cd_poll_fn poll)
{
	CdlFILE file;
	if (!CdSearchFile(&file, name)) {
		printf("cd: %s not found\n", name);
		return NULL;
	}

	uint32_t sectors = ((uint32_t) file.size + SECTOR - 1) / SECTOR;
	uint32_t *buf = mem_level_alloc(sectors * SECTOR);
	if (!buf)
		return NULL;

	CdControl(CdlSetloc, &file.pos, NULL);
	int left = -1;
	if (CdReadRetry((int) sectors, buf, CdlModeSpeed, 3)) {
		if (poll) {
			/* CdReadSync(1) returns the sectors still to come, and must be
			   called regularly for CdReadRetry to retry a failed read */
			while ((left = CdReadSync(1, NULL)) > 0)
				poll((sectors - (uint32_t) left) * SECTOR);
		} else {
			left = CdReadSync(0, NULL);
		}
	}
	if (left < 0) {
		printf("cd: read error on %s\n", name);
		return NULL;
	}

	*size = (uint32_t) file.size;
	return buf;
}
