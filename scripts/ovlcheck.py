#!/usr/bin/env python3
# The load overlay's callers (docs/porting-notes.md §90.7): every function
# engine/load.ld puts in the overlay must be called only by main (the boot's
# inits), by load_level, or by another function of the overlay, through a
# direct call (R_MIPS_26), and must not have its address taken (R_MIPS_HI16 /
# LO16 in code, R_MIPS_32 in data) but the load's two callbacks, which only
# the load passes on. Run at the link on the objects (CMakeLists.txt):
#   ovlcheck.py <objdump> <load.ld> <object>...
# Exit status 1, the offending calls listed, when the overlay could be
# called in play (its room is the stream's pool's then).
import re, subprocess, sys

od, ld = sys.argv[1], sys.argv[2]
objs = sys.argv[3:]
ovl = set()
for l in open(ld):
    m = re.search(r'\((\.text\.[^)]+)\)', l)
    if m:
        ovl.add(re.sub(r'^\.text\.(unlikely\.|startup\.|hot\.)?', '', m.group(1)))
allowed_callers = {'main', 'load_level'}
callbacks = {'loading_screen', 'load_poll'}
bad = []
for o in objs:
    out = subprocess.run([od, '-dr', o], capture_output=True, text=True).stdout
    cur = None
    for l in out.splitlines():
        m = re.match(r'^[0-9a-f]+ <([^>]+)>:', l)
        if m:
            cur = m.group(1)
            continue
        m = re.match(r'^\s+[0-9a-f]+:\s+R_MIPS_(26|HI16|LO16)\s+(\S+)', l)
        if not m or not cur:
            continue
        kind = m.group(1)
        sym = re.sub(r'^\.text\.(unlikely\.|startup\.|hot\.)?', '', m.group(2)).split('+')[0].split('-')[0]
        if sym not in ovl:
            continue
        caller = re.sub(r'^(unlikely\.|startup\.|hot\.)?', '', cur)
        if kind == '26':
            if caller not in ovl and caller not in allowed_callers:
                bad.append('%s calls %s (%s)' % (caller, sym, o.split('/')[-1]))
        elif sym not in callbacks:
            bad.append('%s takes the address of %s (%s)' % (caller, sym, o.split('/')[-1]))
    out = subprocess.run([od, '-r', o], capture_output=True, text=True).stdout
    sec = None
    for l in out.splitlines():
        m = re.match(r'^RELOCATION RECORDS FOR \[([^\]]+)\]', l)
        if m:
            sec = m.group(1)
            continue
        m = re.match(r'^[0-9a-f]+\s+R_MIPS_32\s+(\S+)', l)
        if m and sec and not sec.startswith('.text') and not sec.startswith('.debug') and not sec.startswith('.pdr'):
            sym = re.sub(r'^\.text\.(unlikely\.|startup\.|hot\.)?', '', m.group(1)).split('+')[0]
            if sym in ovl and sym not in callbacks:
                bad.append('%s holds the address of %s (%s)' % (sec, sym, o.split('/')[-1]))
if bad:
    print('ovlcheck: the load overlay could run in play (engine/load.ld):')
    for b in sorted(set(bad)):
        print('  ' + b)
    sys.exit(1)
print('ovlcheck: %d overlay functions, called from the boot and the loads only' % len(ovl))
