#!/usr/bin/env python3
# PCSX2 PS4 port (AI-assisted): reads /data/PCSX2/eeprof.bin (ps5/coreorbis/orbis-shims/orbis_eeprof.cpp) and says where
# the profiled thread's time went: the eboot's functions, the recompiled code by kind (EE/IOP/VU0/VU1/VIF/dispatchers)
# and the PS2 blocks within it, and the system libraries with the eboot code that called them.
#
#   eeprof.py eeprof.bin --elf pcsx2 [--top 40] [--from N] [--to N]
#
# --from/--to limit the analysis to sample indices (the boot log's "[eeprof] n=" marks tie them to seconds).
# The ELF's symbols come from llvm-nm (llvm-nm-21 or llvm-nm on PATH). Needs proper testing.
import argparse, bisect, collections, shutil, struct, subprocess, sys

HEADER = struct.Struct('<8sIIQQQQQQ24x')
RECORD = struct.Struct('<IIQ')
JIT = struct.Struct('<QQII8s32s')
LIB_WORDS = 6
LIB = struct.Struct('<Q%dQ' % LIB_WORDS)
LIB_LO, LIB_HI = 0x800000000, 0x900000000


def load(path):
    data = open(path, 'rb').read()
    magic, version, rip_off, code_base, code_size, self_fn, ee_stack, period, thread = HEADER.unpack_from(data, 0)
    if magic[:7] != b'EEPROF1':
        sys.exit('not an eeprof.bin')
    pos = HEADER.size
    samples, jit, libs = {}, [], {}
    while pos + RECORD.size <= len(data):
        typ, count, first = RECORD.unpack_from(data, pos)
        pos += RECORD.size
        if typ == 1:
            vals = struct.unpack_from('<%dQ' % count, data, pos)
            for i, v in enumerate(vals):
                samples[first + i] = v
            pos += 8 * count
        elif typ == 2:
            for i in range(count):
                ptr, key, size, at, group, name = JIT.unpack_from(data, pos + i * JIT.size)
                jit.append((ptr, size, at, key, group.split(b'\0')[0].decode(errors='replace'),
                            name.split(b'\0')[0].decode(errors='replace')))
            pos += JIT.size * count
        elif typ == 3:
            for i in range(count):
                vals = LIB.unpack_from(data, pos + i * LIB.size)
                libs[vals[0]] = vals[1:]
            pos += LIB.size * count
        else:
            sys.exit('unknown record type %d at %d' % (typ, pos))
    info = dict(version=version, code_base=code_base, code_size=code_size, self_fn=self_fn, period=period, thread=thread)
    return info, samples, jit, libs


def symbols(elf):
    nm = shutil.which('llvm-nm-21') or shutil.which('llvm-nm') or 'nm'
    out = subprocess.run([nm, '-C', '-n', '--defined-only', elf], capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split(' ', 2)
        if len(parts) == 3 and parts[1] in 'tTwW':
            syms.append((int(parts[0], 16), parts[2]))
    syms.sort()
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('bin')
    ap.add_argument('--elf', required=True)
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--from', dest='lo', type=int, default=0)
    ap.add_argument('--to', dest='hi', type=int, default=1 << 62)
    a = ap.parse_args()

    info, samples, jit, libs = load(a.bin)
    syms = symbols(a.elf)
    addrs = [s[0] for s in syms]
    ref = next((addr for addr, name in syms if name.startswith('OrbisEEProfStart(')), None)
    if ref is None:
        sys.exit('no OrbisEEProfStart in the ELF')
    load_bias = info['self_fn'] - ref
    code_lo, code_hi = info['code_base'], info['code_base'] + info['code_size']

    def name_of(rip):
        elf = rip - load_bias
        i = bisect.bisect_right(addrs, elf) - 1
        return syms[i][1] if i >= 0 else '?'

    # JIT registrations, by start address; the latest one registered before a sample covers it.
    jit.sort(key=lambda e: (e[0], e[2]))
    jit_starts = [e[0] for e in jit]

    def jit_of(rip, index):
        i = bisect.bisect_right(jit_starts, rip) - 1
        best = None
        while i >= 0 and rip - jit[i][0] < 1 << 20:
            ptr, size, at, key, group, name = jit[i]
            if ptr <= rip < ptr + size and at <= index and (best is None or at > best[2]):
                best = jit[i]
            i -= 1
        return best

    total = 0
    areas = collections.Counter()
    funcs = collections.Counter()
    groups = collections.Counter()
    blocks = collections.Counter()
    named = collections.Counter()
    lib_callers = collections.Counter()
    copy_callers = collections.Counter()
    for index, rip in samples.items():
        if not (a.lo <= index < a.hi):
            continue
        total += 1
        if code_lo <= rip < code_hi:
            e = jit_of(rip, index)
            if e is None:
                areas['jit (unregistered)'] += 1
                continue
            ptr, size, at, key, group, name = e
            g = group or 'named'
            areas['jit'] += 1
            groups[g] += 1
            if key and g in ('EE', 'IOP', 'VU0', 'VU1'):
                blocks['%s %08x' % (g, key)] += 1
            else:
                named['%s %s' % (g, name or hex(key))] += 1
        elif LIB_LO <= rip < LIB_HI:
            areas['system libraries'] += 1
            words = libs.get(index)
            caller = None
            if words:
                for w in words:
                    if code_lo <= w < code_hi:
                        caller = 'jit'
                        break
                    if 0 <= w - load_bias < (addrs[-1] if addrs else 0):
                        caller = name_of(w)
                        break
            lib_callers[caller or '?'] += 1
        else:
            areas['eboot'] += 1
            fn = name_of(rip)
            funcs[fn] += 1
            # PS4: the eboot's memcpy/memmove samples keep their stack too (orbis_eeprof.cpp, ORBIS_PS4).
            words = libs.get(index)
            if words and fn in ('memcpy', 'memmove'):
                caller = next((name_of(w) for w in words if w and 0 <= w - load_bias < addrs[-1]), '?')
                copy_callers['%s <- %s' % (fn, caller)] += 1

    if not total:
        sys.exit('no samples in range')
    pct = lambda n: 100.0 * n / total
    print('%d samples (%s thread, period %d us)' % (total, ['EE', 'GS', 'VU1'][min(info['thread'], 2)], info['period']))
    print('\nareas:')
    for k, n in areas.most_common():
        print('  %6.2f%%  %s' % (pct(n), k))
    print('\neboot functions (top %d):' % a.top)
    for k, n in funcs.most_common(a.top):
        print('  %6.2f%%  %s' % (pct(n), k[:150]))
    print('\nrecompiled code by kind:')
    for k, n in groups.most_common():
        print('  %6.2f%%  %s' % (pct(n), k))
    print('\nPS2 blocks (top %d):' % a.top)
    for k, n in blocks.most_common(a.top):
        print('  %6.2f%%  %s' % (pct(n), k))
    print('\nnamed JIT code (dispatchers, VIF unpackers, ...):')
    for k, n in named.most_common(15):
        print('  %6.2f%%  %s' % (pct(n), k))
    print('\nmemcpy/memmove samples by caller (PS4 builds):')
    for k, n in copy_callers.most_common(20):
        print('  %6.2f%%  %s' % (pct(n), k[:150]))
    print('\nsystem library samples by caller:')
    for k, n in lib_callers.most_common(15):
        print('  %6.2f%%  %s' % (pct(n), k[:150]))


if __name__ == '__main__':
    main()
