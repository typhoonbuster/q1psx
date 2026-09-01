#include <stdarg.h>
#include <stdio.h>

#include "sys.h"

void Sys_Error(const char *fmt, ...)
{
	char    msg[256];
	va_list args;

	va_start(args, fmt);
	vsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);

	printf("\n*** q1psx Sys_Error: %s ***\n", msg);

	/* nothing sane to do on a console — stop here */
	for (;;)
		__asm__ volatile("");
}
