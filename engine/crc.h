/*
 * q1psx — CRC-16/CCITT (XMODEM), used to fingerprint progs.dat / models / maps.
 * Verbatim from id-Software/Quake WinQuake/crc.{c,h}.
 */

#pragma once

#include "qcommon.h"

void           CRC_Init(unsigned short *crcvalue);
void           CRC_ProcessByte(unsigned short *crcvalue, byte data);
unsigned short CRC_Value(unsigned short crcvalue);

/* convenience: full CRC of a buffer */
unsigned short CRC_Block(const byte *start, int count);
