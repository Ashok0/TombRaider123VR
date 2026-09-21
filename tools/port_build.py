# port_build.py -- carry the PDB build's symbols across to a build shipped without PDBs.
#
#   python tools\port_build.py <dir>
#
# <dir> holds the new tomb123.exe, tomb1.dll, tomb2.dll and tomb3.dll. PDB\ holds
# the old ones with their PDBs, as always. Writes <dir>\port_build.json (read by
# verify_addresses.py) and prints the Engine.h Layout row and the GameDll.cpp
# rows for the new build, with the evidence behind each value.
#
# HOW THE MATCH WORKS
#
# Both images have a .pdata table, which gives every non-leaf function's exact
# extent without any heuristics. Each function is disassembled and reduced to a
# token per instruction -- mnemonic plus operands, with RIP-relative
# displacements and branch/call targets masked out and struct displacements and
# immediates kept. Then:
#
#   1. unique exact token-sequence hashes pair up, then unique mnemonic-only ones;
#   2. equal-length matched pairs are walked instruction by instruction, pairing
#      up call targets (new function matches) and RIP-relative targets (a vote
#      for old global -> new global);
#   3. what is left is paired by difflib similarity >= 0.75, best first, and its
#      aligned 'equal' blocks vote the same way; repeat until nothing new pairs.
#
# A global's new address is the winner of its votes, and the tool prints how
# unanimous that was. A function's is its matched pair, or -- for small leaf
# functions with no .pdata entry -- the votes from `lea` references to it.
#
# None of that is proof. verify_addresses.py re-checks what the mod actually
# depends on (prologue windows, structural relations, the APP vtable slots)
# directly against the new image.
import collections, difflib, hashlib, json, os, sys

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PDB  = os.path.join(ROOT, 'PDB')

IMAGES = ('tomb123.exe', 'tomb1.dll', 'tomb2.dll', 'tomb3.dll')

# What the mod reads, in Layout / GameDllLayout column order.
EXE_LAYOUT = ['vid_setPass', 'validate_draw', 'ogl_draw', 'ogl_present', 'fmvShow',
              'ogl_setRenderTarget', 'gGame', '_XInputGetState', 'vid_state',
              'vid_state_prev', 'mProj', 'mView_packed', 'shaders', 'ogl_textures',
              'FBO_custom', 'FBO_default', 'app', 'gWidth', 'gHeight',
              'gTargetWidth', 'gTargetHeight']
DLL_LAYOUT = ['lara', 'camera', 'room', 'number_rooms', 'draw_rooms',
              'number_draw_rooms', 'w2v_matrix', 'phd_mxptr', 'phd_winxmax',
              'phd_winymax', 'outside', 'outside_left', 'outside_right',
              'outside_top', 'outside_bottom', 'PrintRoomsList',
              'S_GetObjectBounds', 'DrawSkyHD', 'S_InitialisePolyList',
              'phd_GenerateW2V', 'w2v_scene_return', 'frame_frac', 'lara_item',
              'DrawCreatureHD', 'DrawHair', 'gLaraHead', 'gActorHead', 'objects', 'analogInput',
          'input', 'LaraAboveWater', 'AnimateLara', 'GetCollisionInfo', 'UpdateLaraRoom']
# Not a symbol: the return address of the ONE phd_GenerateW2V call that builds
# the main scene view, inside S_InitialisePolyList. FirstPerson.cpp gates on it
# so it rewrites the scene camera and nothing else (inventory, shadows, pickup
# spin, photo mode all call the same function).
DERIVED = {'w2v_scene_return': ('S_InitialisePolyList', 'phd_GenerateW2V')}
# Referenced from the self-checks in verify_addresses.py.
EXE_EXTRA = ['vidInit', 'init_ogl', 'appInit', 'vid_setViewMatrix', 'WinMain',
             '_XInputSetState', 'mShadow']

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True


def pe_stamp(path):
    pe = pefile.PE(path, fast_load=True)
    return pe.FILE_HEADER.TimeDateStamp


def old_symbols(image):
    """name -> (rva, kind, size) from pdbdump.py, preferring real symbols over publics."""
    import subprocess
    out = subprocess.run([sys.executable, os.path.join(HERE, 'pdbdump.py'),
                          os.path.join(PDB, image)],
                         capture_output=True, text=True).stdout
    d = {}
    for line in out.splitlines():
        if not line.startswith('0x'):
            continue
        p = line.split(None, 3)
        if len(p) < 4:
            continue
        name = p[3].strip()
        if name not in d or (d[name][1] == 'public' and p[1] != 'public'):
            d[name] = (int(p[0], 16), p[1], int(p[2]))
    return d


class Image:
    def __init__(self, path):
        pe = pefile.PE(path)
        self.data = pe.get_memory_mapped_image()
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.stamp = pe.FILE_HEADER.TimeDateStamp
        self.size = pe.OPTIONAL_HEADER.SizeOfImage
        self.funcs = {}
        for e in pe.DIRECTORY_ENTRY_EXCEPTION:
            b, end = e.struct.BeginAddress, e.struct.EndAddress
            if b in self.funcs:
                continue
            ins = self._tokens(b, end)
            self.funcs[b] = dict(
                end=end, ins=ins,
                h=hashlib.md5('\n'.join(i[1] for i in ins).encode()).hexdigest(),
                hl=hashlib.md5('\n'.join(i[1].split(' ')[0] for i in ins).encode()).hexdigest())

    def _tokens(self, start, end):
        """[(rva, token, call target, rip target)]"""
        out = []
        for ins in md.disasm(bytes(self.data[start:end]), self.base + start):
            call = rip = None
            ops = []
            for op in ins.operands:
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                    rip = ins.address + ins.size + op.mem.disp - self.base
                    ops.append('m[rip]')
                elif op.type == X86_OP_MEM:
                    ops.append('m[%d,%d,%d,%d]' % (op.mem.base, op.mem.index,
                                                   op.mem.scale, op.mem.disp))
                elif op.type == X86_OP_IMM and (ins.group(1) or ins.group(2) or ins.group(7)):
                    if ins.mnemonic == 'call':
                        call = op.imm - self.base
                    ops.append('T')
                elif op.type == X86_OP_IMM:
                    ops.append('i%d' % op.imm)
                else:
                    ops.append('r%d' % op.reg)
            out.append((ins.address - self.base, ins.mnemonic + ' ' + ','.join(ops), call, rip))
        return out


def match(old, new):
    """Returns (fmap old->new, fuzzy old->ratio, dvotes old->Counter(new))."""
    fmap, fuzzy = {}, {}
    dvotes = collections.defaultdict(collections.Counter)

    for key in ('h', 'hl'):
        oi, ni = collections.defaultdict(list), collections.defaultdict(list)
        for b, f in old.funcs.items():
            oi[f[key]].append(b)
        for b, f in new.funcs.items():
            ni[f[key]].append(b)
        used = set(fmap.values())
        for h, ol in oi.items():
            nl = ni.get(h, [])
            if len(ol) == 1 and len(nl) == 1 and ol[0] not in fmap and nl[0] not in used:
                fmap[ol[0]] = nl[0]
                used.add(nl[0])

    def pair_up(a_ins, b_ins, used):
        new_pairs = 0
        for a, b in zip(a_ins, b_ins):
            if a[2] is not None and b[2] is not None and a[2] in old.funcs \
                    and b[2] in new.funcs and a[2] not in fmap and b[2] not in used:
                fmap[a[2]] = b[2]
                used.add(b[2])
                new_pairs += 1
            if a[3] is not None and b[3] is not None:
                dvotes[a[3]][b[3]] += 1
        return new_pairs

    def align(ob, nb, used):
        a, b = old.funcs[ob]['ins'], new.funcs[nb]['ins']
        if len(a) == len(b) and all(x[1].split(' ')[0] == y[1].split(' ')[0]
                                    for x, y in zip(a, b)):
            return pair_up(a, b, used)
        sm = difflib.SequenceMatcher(None, [i[1] for i in a], [i[1] for i in b],
                                     autojunk=False)
        n = 0
        for tag, a0, a1, b0, b1 in sm.get_opcodes():
            if tag == 'equal':
                n += pair_up(a[a0:a1], b[b0:b1], used)
        return n

    aligned = set()

    def propagate():
        used = set(fmap.values())
        while True:
            progress = 0
            for ob, nb in list(fmap.items()):
                if ob not in aligned:
                    aligned.add(ob)
                    progress += align(ob, nb, used)
            if not progress:
                return

    propagate()
    while True:
        used = set(fmap.values())
        un = [b for b in new.funcs if b not in used]
        cands = []
        for ob, of in old.funcs.items():
            if ob in fmap or len(of['ins']) < 6:
                continue
            ta = [i[1] for i in of['ins']]
            best = (0.0, None)
            for nb in un:
                ln = len(new.funcs[nb]['ins'])
                if not (0.6 * len(ta) <= ln <= 1.6 * len(ta)):
                    continue
                sm = difflib.SequenceMatcher(None, ta, [i[1] for i in new.funcs[nb]['ins']],
                                             autojunk=False)
                if sm.real_quick_ratio() < 0.75 or sm.quick_ratio() < 0.75:
                    continue
                r = sm.ratio()
                if r > best[0]:
                    best = (r, nb)
            if best[0] >= 0.75:
                cands.append((best[0], ob, best[1]))
        added = 0
        for r, ob, nb in sorted(cands, reverse=True):
            if ob not in fmap and nb not in used:
                fmap[ob] = nb
                fuzzy[ob] = r
                used.add(nb)
                added += 1
        if not added:
            break
        propagate()

    return fmap, fuzzy, dvotes


def resolve(image, newdir):
    old = Image(os.path.join(PDB, image))
    new = Image(os.path.join(newdir, image))
    syms = old_symbols(image)
    fmap, fuzzy, dvotes = match(old, new)

    want = EXE_LAYOUT + EXE_EXTRA if image.endswith('.exe') else DLL_LAYOUT
    out = {}
    for name in want:
        if name in DERIVED:
            continue
        if name not in syms:
            out[name] = dict(new=0, how='absent from the PDB')
            continue
        rva = syms[name][0]
        if rva in fmap:
            how = ('fuzzy %.2f' % fuzzy[rva]) if rva in fuzzy else 'function match'
            out[name] = dict(old=rva, new=fmap[rva], how=how)
        elif rva in dvotes:
            c = dvotes[rva]
            n, v = c.most_common(1)[0]
            out[name] = dict(old=rva, new=n, how='%d/%d votes' % (v, sum(c.values())))
        else:
            out[name] = dict(old=rva, new=None, how='UNRESOLVED')

    # Fallback for a global nothing references RIP-relatively in a matched
    # function (mView_packed is only ever reached as vid_state.view): if the
    # nearest resolved data symbols below AND above it both moved by the same
    # amount, it moved by that amount too. Labelled as inferred, and only
    # accepted when both neighbours agree.
    resolved = sorted((r, dvotes[r].most_common(1)[0][0] - r, n)
                      for n, (r, kind, _sz) in syms.items()
                      if kind == 'data' and r in dvotes)
    for name, e in out.items():
        if e['new'] is not None:
            continue
        lo = [x for x in resolved if x[0] < e['old']]
        hi = [x for x in resolved if x[0] > e['old']]
        if lo and hi and lo[-1][1] == hi[0][1]:
            e['new'] = e['old'] + lo[-1][1]
            e['how'] = 'inferred: %s and %s both shift %+d' % (lo[-1][2], hi[0][2], lo[-1][1])
    # Derived call sites.
    #
    # MSVC splits these functions across several .pdata chunks, so the call is
    # usually not in the chunk that carries the symbol. The old call site is
    # found by scanning the old image inside the symbol's full extent; the new
    # one is then read off the instruction alignment of whichever matched chunk
    # pair contains it.
    for name, (caller, callee) in DERIVED.items():
        if name not in want:
            continue          # not part of this image's table (the exe has none)
        if caller not in syms or callee not in syms:
            out[name] = dict(new=None, how='UNRESOLVED (no %s symbol)' % caller)
            continue
        lo, size = syms[caller][0], syms[caller][2]
        target = syms[callee][0]
        sites = [(c, i) for c in old.funcs
                 for i in old.funcs[c]['ins']
                 if i[2] == target and lo <= i[0] < lo + size]
        if len(sites) != 1:
            out[name] = dict(new=None, how='UNRESOLVED (%d call sites in %s)' % (len(sites), caller))
            continue
        chunk, ins = sites[0]
        if chunk not in fmap:
            out[name] = dict(new=None, how='UNRESOLVED (chunk 0x%X unmatched)' % chunk)
            continue
        a, b = old.funcs[chunk]['ins'], new.funcs[fmap[chunk]]['ins']
        sm = difflib.SequenceMatcher(None, [i[1] for i in a], [i[1] for i in b], autojunk=False)
        k = a.index(ins)
        hit = None
        for tag, a0, a1, b0, b1 in sm.get_opcodes():
            if tag == 'equal' and a0 <= k < a1:
                hit = b[b0 + (k - a0)]
        if hit is None or hit[2] != out[callee]['new']:
            out[name] = dict(new=None, how='UNRESOLVED (no aligned call in the new chunk)')
            continue
        out[name] = dict(old=ins[0] + 5, new=hit[0] + 5,
                         how='return address of the %s call in %s' % (callee, caller))

    print('  %s: %d/%d functions paired, %d globals voted'
          % (image, len(fmap), len(old.funcs), len(dvotes)), file=sys.stderr)
    return dict(timestamp=new.stamp, size_of_image=new.size, symbols=out)


def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: python tools\\port_build.py <dir>')
    newdir = os.path.abspath(sys.argv[1])
    result = {}
    for image in IMAGES:
        print('matching %s ...' % image, file=sys.stderr)
        result[image] = resolve(image, newdir)

    path = os.path.join(newdir, 'port_build.json')
    with open(path, 'w') as f:
        json.dump(result, f, indent=1)

    bad = 0
    for image in IMAGES:
        r = result[image]
        print('\n// %s  PE 0x%08X' % (image, r['timestamp']))
        for name, e in r['symbols'].items():
            if e['new'] is None:
                bad += 1
            print('//   %-22s %-12s %s' % (name,
                                          'UNRESOLVED' if e['new'] is None else '0x%08X' % e['new'],
                                          e['how']))
    if bad:
        print('\n%d symbol(s) UNRESOLVED -- read the disassembly by hand' % bad)
        sys.exit(1)

    v = lambda img, n: '0x%08X' % result[img]['symbols'][n]['new']
    exe = result['tomb123.exe']
    print('\n// ---- src/Engine.h ----')
    print('constexpr Layout kBuildNEW = {')
    print('    "TR I-III Remastered (PE 0x%08X, no PDB)", 0x%08X,' % (exe['timestamp'], exe['timestamp']))
    for n in EXE_LAYOUT:
        print('    /* %-19s */ %s,' % (n.lstrip('_'), v('tomb123.exe', n)))
    print('};')
    print('\n// ---- src/GameDll.cpp ----')
    for img, title in (('tomb1.dll', 'Tomb Raider I'), ('tomb2.dll', 'Tomb Raider II'),
                       ('tomb3.dll', 'Tomb Raider III')):
        print('        { L"%s", "%s", 0x%08X,' % (img, title, result[img]['timestamp']))
        for i, n in enumerate(DLL_LAYOUT):
            print('          /* %-15s */ %s%s' % (n[:15], v(img, n),
                                                  ' },' if i == len(DLL_LAYOUT) - 1 else ','))
    print('\nwrote %s' % path)


if __name__ == '__main__':
    main()
