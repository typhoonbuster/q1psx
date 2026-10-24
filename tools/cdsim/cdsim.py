#!/usr/bin/env python3
"""
q1psx — cdsim: the drive's journal replayed on a disc layout
(docs/porting-notes.md §83).

The bench's journal (`CDLOG=1 scripts/bench.sh`, engine/cd.c) has a `cd`
line per read: who read (D a directory or a header, L a whole file, Z a
world chunk, M a streamed model, S a streamed sound's piece, U the music's
refill), its tag, its first sector, its sectors, where the head came from,
the vblanks it started and ended at, the seek the engine's model gave it
and the polls the model held it; `cd # <phase>` lines name the phases (a
map's load, its walk, the end). This tool charges every read a seek by a
drive model and the sectors' transfer, per phase, and can first move every
read to another layout of the same files: a read is a (file, offset)
through the disc the journal was made on, and lands where that file is on
the other disc (the nearest of its copies, if the layout duplicates it).

    cdsim.py <bench.log> --disc <q1psx.bin>
             [--layout <other.bin | table.txt>] [--model duck|redux]
             [--per-read] [--check]

  --disc    the image the journal was recorded on: its files' sectors
            (a 2352-byte-sector image; the ISO9660 directories are read)
  --layout  the layout to replay on: another image, or a table of lines
            `<lba> <sectors> <bytes> <path>` (`cdsim.py --table <bin>`
            writes one); a path may appear several times (copies), and a
            `<path>@<sector>` entry holds the file from that sector of it
            on (a track cut in parts around a level's data); a `<path>~`
            entry is a condensed copy (a level's subset of the streamed
            assets: a read lands in it at its offset scaled to its size);
            default: the recording disc itself
  --model   duck: DuckStation's seek model (src/core/cdrom.cpp
            GetTicksForSeek, from measures of the console's mechanism; the
            engine's CDMODEL=1 is its port); redux: PCSX-Redux's (3 µs a
            sector of distance plus 1 ms); default duck
  --no-dir  leaves the directory and header reads out (a layout whose
            levels carry their own table of sectors reads none)
  --check   compares the engine's seek figure on each line with this
            tool's (the port against the model: the largest difference)
  --per-read prints every read after its line: its file, the seek charged

Per phase: the reads by who, their sectors, the seek time charged, the
transfer time, the drive's busy share of the phase's vblanks, the hops by
class (near: under 330 sectors; mid: under 7200; sled: farther) and the
longest, and the music's refills' share.
"""
import argparse
import math
import struct
import sys

RAW = 2352
HDR = 24                              # sync, header, subheader: a mode 2 form 1 sector's data at 24
SECTOR_S = 1 / 150.0                  # a sector at double speed
VBL_S = 1 / 59.94


# ------------------------------------------------------------ the discs --

def read_sector(f, lba):
    f.seek(lba * RAW + HDR)
    return f.read(2048)


def le32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def walk_dir(f, lba, length, path, out):
    data = b''.join(read_sector(f, lba + i) for i in range((length + 2047) // 2048))
    o = 0
    while o < len(data):
        rl = data[o]
        if rl == 0:
            o = (o // 2048 + 1) * 2048
            continue
        nl = data[o + 32]
        name = data[o + 33:o + 33 + nl]
        flags = data[o + 25]
        elba, esize = le32(data, o + 2), le32(data, o + 10)
        if not (nl == 1 and name[0] <= 1):
            n = name.decode('ascii', 'replace').split(';')[0]
            if flags & 2:
                walk_dir(f, elba, esize, path + n + '/', out)
            else:
                out.append((elba, (esize + 2047) // 2048, esize, path + n))
        o += rl


def disc_table(path):
    """every file of a 2352-byte-sector image: (lba, sectors, bytes, path), by lba"""
    out = []
    with open(path, 'rb') as f:
        pvd = read_sector(f, 16)
        if pvd[1:6] != b'CD001':
            sys.exit(f'{path}: no ISO9660 volume')
        root_lba, root_len = le32(pvd, 156 + 2), le32(pvd, 156 + 10)
        # the directories themselves, so a directory's read finds its file
        walk_dir(f, root_lba, root_len, '/', out)
        out.append((root_lba, (root_len + 2047) // 2048, root_len, '/<root dir>'))
        data = b''.join(read_sector(f, root_lba + i) for i in range((root_len + 2047) // 2048))
        o = 0
        while o < len(data):
            rl = data[o]
            if rl == 0:
                o = (o // 2048 + 1) * 2048
                continue
            nl = data[o + 32]
            name = data[o + 33:o + 33 + nl]
            if data[o + 25] & 2 and not (nl == 1 and name[0] <= 1):
                out.append((le32(data, o + 2), (le32(data, o + 10) + 2047) // 2048, le32(data, o + 10),
                            '/' + name.decode('ascii', 'replace') + '/<dir>'))
            o += rl
    out.sort()
    return out


def table_file(path):
    out = []
    for line in open(path):
        if line.startswith('#') or not line.strip():
            continue
        lba, secs, size, p = line.split(None, 3)
        out.append((int(lba), int(secs), int(size), p.strip()))
    out.sort()
    return out


class Layout:
    def __init__(self, table):
        self.table = table
        self.by_path = {}         # path: [(lba, sectors, first sector of the file held, condensed)]
        for lba, secs, size, p in table:
            first, condensed = 0, False
            if p.endswith('~'):
                p, condensed = p[:-1], True
            if '@' in p:
                p, first = p.rsplit('@', 1)
                first = int(first)
            self.by_path.setdefault(p, []).append((lba, secs, first, condensed))

    def size(self, path):
        """the file's sectors (its whole copies' largest)"""
        return max((secs for lba, secs, first, c in self.by_path.get(path, ()) if not c and first == 0), default=0)

    def file_at(self, lba):
        """(path, offset) of a sector, or (None, lba) off every file"""
        lo, hi = 0, len(self.table)
        while lo < hi:
            m = (lo + hi) // 2
            if self.table[m][0] <= lba:
                lo = m + 1
            else:
                hi = m
        if lo:
            flba, secs, size, p = self.table[lo - 1]
            if lba < flba + secs:
                first = int(p.rsplit('@', 1)[1]) if '@' in p else 0
                return p.rsplit('@', 1)[0], lba - flba + first
        return None, lba

    def place(self, path, offset, near, full):
        """the sector of (path, offset) on this layout: the copy holding it nearest `near`
        (`full`: the file's sectors, for a condensed copy's scaling)"""
        copies = [lba + (offset * secs // full if c else offset - first)
                  for lba, secs, first, c in self.by_path.get(path, ())
                  if c or first <= offset < first + secs]
        if not copies:
            return None
        return min(copies, key=lambda x: abs(x - near))


# -------------------------------------------------------------- the models --

def sectors_per_track(lba):
    mm = lba // 4500
    for limit, spt in ((0, 8), (4, 9), (7, 10), (11, 11), (16, 12), (23, 13), (27, 14), (32, 15), (39, 16),
                       (44, 17), (52, 18), (60, 19), (67, 20), (74, 21)):
        if mm <= limit:
            return spt
    return 22


def seek_duck(cur, new):
    """DuckStation's GetTicksForSeek in seconds, double speed, the motor on, no jitter"""
    d = abs(new - cur)
    spt = sectors_per_track(cur)
    tjump = cur - spt if cur >= spt else 0
    if cur < new and d <= spt:
        return SECTOR_S * max(d, 2)
    if cur >= new and tjump <= new:
        return SECTOR_S * max(new - tjump, 1)
    if d < 7200:
        switch = 330.0 - 63.1333 * math.log(min(max(cur / 4500.0, 1.0), 72.0))
        return 0.05 if d < switch else 0.1
    fixed, var, w, maxlba = 0.05, 0.85, 0.4, 72 * 4500.0
    return fixed + var * (math.log(d) / math.log(maxlba)) * w + var * (d / maxlba) * (1 - w)


def seek_redux(cur, new):
    """PCSX-Redux's computeSeekDelay for a read, seconds"""
    return abs(new - cur) * 3e-6 + 1e-3


MODELS = {'duck': seek_duck, 'redux': seek_redux}
WHO = {'D': 'directories', 'L': 'files', 'Z': 'chunks', 'M': 'models', 'S': 'sounds', 'U': 'music'}


# -------------------------------------------------------------- the replay --

def hop_class(d):
    return 'near' if d < 330 else 'mid' if d < 7200 else 'sled'


class Phase:
    def __init__(self, name, vbl0):
        self.name = name
        self.vbl0 = vbl0
        self.vbl1 = vbl0
        self.reads = {w: 0 for w in WHO}
        self.sectors = {w: 0 for w in WHO}
        self.seek = {w: 0.0 for w in WHO}
        self.hops = {'near': 0, 'mid': 0, 'sled': 0}
        self.hop_max = 0
        self.held = 0
        self.engine_vbl = 0           # the vblanks the engine's reads took (start to end)
        self.cut = 0
        self.errors = 0

    def transfer(self):
        return sum(self.sectors.values()) * SECTOR_S

    def seek_total(self):
        return sum(self.seek.values())

    def report(self, out):
        n = sum(self.reads.values())
        span = (self.vbl1 - self.vbl0) * VBL_S
        busy = self.seek_total() + self.transfer()
        who = ' '.join(f'{w} {self.reads[w]}/{self.sectors[w]}' for w in WHO if self.reads[w])
        out(f'{self.name:>12s}: {n:4d} reads {sum(self.sectors.values()):6d} sectors in {span:6.1f} s; '
            f'seeks {self.seek_total():6.2f} s, transfer {self.transfer():6.2f} s, busy {100 * busy / span if span else 0:5.1f} %; '
            f'hops near {self.hops["near"]} mid {self.hops["mid"]} sled {self.hops["sled"]}, longest {self.hop_max}; '
            f'music seeks {self.seek["U"]:5.2f} s; {who}'
            + (f'; {self.cut} cut' if self.cut else '') + (f'; {self.errors} errors' if self.errors else ''))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log', nargs='?')
    ap.add_argument('--disc', help='the image the journal was recorded on')
    ap.add_argument('--layout', help='the layout to replay on: an image or a table')
    ap.add_argument('--model', default='duck', choices=MODELS)
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--no-dir', action='store_true')
    ap.add_argument('--per-read', action='store_true')
    ap.add_argument('--table', help='write the files of this image as a layout table and stop')
    a = ap.parse_args()

    if a.table:
        for lba, secs, size, p in disc_table(a.table):
            print(f'{lba:7d} {secs:6d} {size:10d} {p}')
        return
    if not a.log or not a.disc:
        ap.error('a journal and --disc are needed')

    rec = Layout(disc_table(a.disc))
    if a.layout:
        lay = Layout(table_file(a.layout) if a.layout.endswith('.txt') else disc_table(a.layout))
    else:
        lay = rec
    model = MODELS[a.model]

    phases, cur = [], None
    head = 0                  # the modeled head on the layout
    worst_check, missing = 0.0, 0
    for line in open(a.log, errors='replace'):
        if line.startswith('cd # '):
            name = line[5:].strip()
            vbl = phases[-1].vbl1 if phases else 0
            cur = Phase(name, vbl)
            phases.append(cur)
            continue
        if not line.startswith('cd ') or line.startswith('cd sum'):
            continue
        f = line.split()
        if len(f) != 11:
            continue
        who, tag, lba, n, frm, v0, v1, seek_us, held, st = f[1], int(f[2]), int(f[3]), int(f[4]), int(f[5]), \
            int(f[6]), int(f[7]), int(f[8]), int(f[9]), f[10]
        if cur is None:
            cur = Phase('boot', v0)
            phases.append(cur)
        cur.vbl1 = max(cur.vbl1, v1)
        if a.no_dir and who == 'D':
            continue
        # the read on the layout
        path, ofs = rec.file_at(lba)
        if lay is rec or path is None:
            to = lba
        else:
            to = lay.place(path, ofs, head, rec.size(path))
            if to is None:
                missing += 1
                to = lba
        s = model(head, to)
        d = abs(to - head)
        cur.reads[who] += 1
        cur.sectors[who] += n
        cur.seek[who] += s
        cur.hops[hop_class(d)] += 1
        cur.hop_max = max(cur.hop_max, d)
        cur.held += held
        cur.engine_vbl += v1 - v0
        cur.cut += st == 'x'
        cur.errors += st == 'e'
        if a.check and lay is rec:
            worst_check = max(worst_check, abs(seek_duck(frm, lba) - seek_us / 1e6))
        if a.per_read:
            print(f'{line.rstrip()}   -> {path or "?"}+{ofs} at {to}, seek {1000 * s:6.1f} ms')
        head = to + n

    out = print
    out(f'# {a.log} on {a.layout or a.disc}, the {a.model} model')
    for p in phases:
        p.report(out)
    # the totals of the walks and of the loads
    for kind, pick in (('loads', lambda p: p.name not in ('walk', 'end', 'boot')), ('walks', lambda p: p.name == 'walk')):
        sel = [p for p in phases if pick(p)]
        if not sel:
            continue
        reads = sum(sum(p.reads.values()) for p in sel)
        secs = sum(sum(p.sectors.values()) for p in sel)
        seek = sum(p.seek_total() for p in sel)
        xfer = sum(p.transfer() for p in sel)
        span = sum((p.vbl1 - p.vbl0) * VBL_S for p in sel)
        music = sum(p.seek['U'] for p in sel)
        hops = {k: sum(p.hops[k] for p in sel) for k in ('near', 'mid', 'sled')}
        out(f'{kind:>12s}: {len(sel)} phases, {reads} reads, {secs} sectors, {span:.1f} s; seeks {seek:.2f} s '
            f'({100 * seek / span if span else 0:.1f} % of the time, the music\'s {music:.2f} s), transfer {xfer:.2f} s '
            f'({100 * xfer / span if span else 0:.1f} %); hops near {hops["near"]} mid {hops["mid"]} sled {hops["sled"]}')
    if a.check:
        out(f'# the engine\'s seek figures against this model: the largest difference {1000 * worst_check:.2f} ms')
    if missing:
        out(f'# {missing} reads of files the layout lacks (left where they were)')


if __name__ == '__main__':
    main()
