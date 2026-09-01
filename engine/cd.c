#include <stdint.h>
#include <psxcd.h>

#include "cd.h"

#define CD_SECTOR 2048

void cd_init(void)
{
	CdInit();
}

int cd_load(const char *name, void *buf, int buf_cap)
{
	CdlFILE file;
	if (!CdSearchFile(&file, name))
		return -1;

	int padded = (file.size + (CD_SECTOR - 1)) & ~(CD_SECTOR - 1);
	if (padded > buf_cap)
		return -1;

	CdControl(CdlSetloc, &file.pos, 0);
	CdRead(padded / CD_SECTOR, (uint32_t *) buf, CdlModeSpeed);
	if (CdReadSync(0, 0) < 0)
		return -1;

	return file.size;
}
