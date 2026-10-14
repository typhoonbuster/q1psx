/*
 * q1psx — boot + main loop.
 *
 * R1.1: video core + cycle profiler. The frame is empty apart from the debug
 * overlay; the loop measures what an empty frame costs and prints it to the
 * console (stdout) once a second.
 */

#include <stdint.h>
#include <stdio.h>
#include <psxgpu.h>

#include "prof.h"
#include "vid.h"

int main(void)
{
	vid_init();
	prof_init();
	vid_set_bg(24, 24, 32);

	printf("q1psx: R1.1 video core\n");

	uint32_t frame = 0;
	uint32_t work  = 0;   /* CPU cycles of the last frame's own work */
	char     line[96];

	for (;;) {
		uint32_t t0 = prof_now();

		snprintf(line, sizeof line,
		         "q1psx  R1.1\n\n"
		         "work %6u cyc\nsync %6u cyc\nvbl  %6u cyc\n%u vbl/frame  %u B",
		         work, vid_stats.sync_cycles, vid_stats.vsync_cycles,
		         vid_stats.vbl, vid_stats.pkt_bytes);
		vid_text(16, 16, line);

		uint32_t t1 = prof_now();
		work = prof_cycles(t0, t1);
		vid_flip();

		if ((++frame & 127) == 0)
			printf("frame %u: work %u cyc  gpu-wait %u  vbl-wait %u  %u vbl  %u B\n",
			       frame, work, vid_stats.sync_cycles, vid_stats.vsync_cycles,
			       vid_stats.vbl, vid_stats.pkt_bytes);
	}
}
