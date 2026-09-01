#include <stdint.h>
#include <string.h>
#include <psxcd.h>

#include "sys.h"
#include "zone.h"
#include "cd.h"

#define CD_SECTOR 2048

void cd_init(void)
{
	CdInit();
}

int cd_find(const char *name, cd_file_t *out)
{
	return CdSearchFile(out, name) ? 1 : 0;
}

int cd_read_at(const cd_file_t *file, int offset, void *buf, int len)
{
	if (offset < 0 || len < 0 || offset + len > file->size + CD_SECTOR)
		return -1;

	int base    = CdPosToInt(&file->pos);
	int within  = offset % CD_SECTOR;
	int nsec    = (within + len + CD_SECTOR - 1) / CD_SECTOR;

	int      hmark  = Hunk_HighMark();
	uint32_t *bounce = Hunk_HighAllocName(nsec * CD_SECTOR, "cdscr");
	if (!bounce)
		return -1;

	CdlLOC loc;
	CdIntToPos(base + offset / CD_SECTOR, &loc);
	CdControl(CdlSetloc, &loc, 0);
	CdRead(nsec, bounce, CdlModeSpeed);
	int rc = CdReadSync(0, 0);

	if (rc >= 0)
		memcpy(buf, (uint8_t *) bounce + within, len);

	Hunk_FreeToHighMark(hmark);
	return rc < 0 ? -1 : len;
}

int cd_load(const char *name, void *buf, int buf_cap)
{
	cd_file_t file;
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
