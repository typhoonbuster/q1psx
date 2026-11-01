#!/usr/bin/env python3
# The disc's system area as a pressed disc lays it out (docs/porting-notes.md
# §123.3; psx-spx, CDROM Disk Format: sectors 0-11 Mode 2 Form 1, the licence
# string in 4 and the logo in 5-11, sectors 12-15 Form 2). mkpsxiso, given no
# licence file, writes all sixteen as empty Form 2 sectors; Mednafen reads
# sectors 4-11 as Form 1 (their EDC), finds them unreadable and takes the
# image for no PS1 disc at all: its BIOS goes to the CD player and its drive
# refuses the data reads. Sectors 0-11 are written again here as empty Form 1
# sectors, their EDC and ECC computed: no licence string, no logo (Sony's
# data, never on this disc). Run at the build on the image (CMakeLists.txt):
#   sysarea.py <image.bin>
# A MODE2/2352 image whose sector 16 is the volume descriptor; the ECC code is
# checked on that sector first, the image left as it is when it disagrees.
import sys

EDC = []
for i in range(256):
    c = i
    for _ in range(8):
        c = (c >> 1) ^ 0xD8018001 if c & 1 else c >> 1
    EDC.append(c)

def edc(data):
    c = 0
    for x in data:
        c = EDC[(c ^ x) & 0xFF] ^ (c >> 8)
    return c

F, B = [0] * 256, [0] * 256              # GF(2^8) steps of the RSPC code
for i in range(256):
    j = ((i << 1) ^ (0x11D if i & 0x80 else 0)) & 0xFF
    F[i] = j
    B[i ^ j] = i

def parity(s, majors, minors, major_mult, minor_inc, dest):
    size = majors * minors
    for major in range(majors):
        index = (major >> 1) * major_mult + (major & 1)
        a = b = 0
        for _ in range(minors):
            t = s[12 + index]
            index += minor_inc
            if index >= size:
                index -= size
            a ^= t
            b ^= t
            a = F[a]
        a = B[F[a] ^ b]
        s[dest + major] = a
        s[dest + major + majors] = a ^ b

def form1(s, subheader):
    """sector s (2352 bytes, its sync and header kept) as a Form 1 sector
    holding its user data: the subheader, the EDC, the P and Q parity (the
    header counted as zeros, as Mode 2 does)"""
    s[16:24] = subheader
    s[0x818:0x81C] = edc(s[16:0x818]).to_bytes(4, 'little')
    header = bytes(s[12:16])
    s[12:16] = bytes(4)
    parity(s, 86, 24, 2, 86, 0x81C)      # P
    parity(s, 52, 43, 86, 88, 0x8C8)     # Q
    s[12:16] = header

with open(sys.argv[1], 'r+b') as f:
    f.seek(16 * 2352)
    pvd = f.read(2352)
    test = bytearray(pvd)
    test[0x818:] = bytes(2352 - 0x818)
    form1(test, pvd[16:24])
    if bytes(test) != pvd or pvd[24:30] != b'\x01CD001':
        sys.exit('sysarea: sector 16 is not a Form 1 volume descriptor this code reproduces; image left as it is')
    for lba in range(12):
        f.seek(lba * 2352)
        s = bytearray(f.read(2352))
        s[24:] = bytes(2352 - 24)
        form1(s, bytes([0, 0, 8, 0, 0, 0, 8, 0]))   # (file 0, channel 0, data, no coding)
        f.seek(lba * 2352)
        f.write(s)
