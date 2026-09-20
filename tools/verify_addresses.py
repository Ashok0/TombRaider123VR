# verify_addresses.py -- prove src/Engine.h and src/GameDll.cpp agree with the PDBs.
#
# Every constant in those files for the build in PDB\ was produced by
# pdbdump.py/typedump.py. This re-derives them from the PDBs and diffs, so a
# typo, a stale edit, or a game patch is caught here rather than by a crash in
# the game.
#
# Rows for builds shipped WITHOUT PDBs were carried across by port_build.py.
# Name the directory holding each such build (default: retail\ and gold\,
# whichever are present) and its rows are checked against port_build.json and
# against the images
# themselves -- prologues, structural relations, the APP vtable slots, the
# consts bit tests and the view-matrix writes.
#
#   python tools\verify_addresses.py [nopdb-build-dir ...]
#
# Exit code 0 = everything matches. Non-zero = at least one mismatch.
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PDB  = os.path.join(ROOT, 'PDB')

def syms(image):
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
        try:
            rva = int(p[0], 16)
        except ValueError:
            continue
        name = p[3].strip()
        # Prefer the first (lowest) definition and never let a public symbol
        # shadow the real function/data symbol of the same name.
        if name not in d or (d[name][1] == 'public' and p[1] != 'public'):
            d[name] = (rva, p[1], int(p[2]))
    return d

def udt(image, typename):
    out = subprocess.run([sys.executable, os.path.join(HERE, 'typedump.py'),
                          os.path.join(PDB, image), typename],
                         capture_output=True, text=True).stdout
    size = None
    fields = {}
    for line in out.splitlines():
        m = re.match(r'struct \S+ \{\s+// (\d+) bytes', line)
        if m:
            size = int(m.group(1))
        m = re.match(r'\s*/\*\s*(-?\d+) \*/ \S+\s+(\S+)', line)
        if m:
            fields[m.group(2)] = int(m.group(1))
    return size, fields

def consts(path):
    """Pull `name = 0xHEX;` constants out of a C++ source file."""
    txt = open(path, encoding='utf-8', errors='replace').read()
    return {m.group(1): int(m.group(2), 16)
            for m in re.finditer(r'\b(\w+)\s*=\s*(0x[0-9A-Fa-f]+)\s*[;,]', txt)}

fails = []
checks = 0

def check(what, got, want):
    global checks
    checks += 1
    if got != want:
        fails.append('%-46s source=%s  pdb=%s' %
                     (what,
                      hex(got)  if isinstance(got, int)  else got,
                      hex(want) if isinstance(want, int) else want))

# ---------------------------------------------------------------- tomb123.exe
print('=== tomb123.exe: function and data RVAs (src/Engine.h) ===')
s = syms('tomb123.exe')
h = consts(os.path.join(ROOT, 'src', 'Engine.h'))

EXE = ['vid_setPass', 'validate_draw', 'ogl_draw', 'ogl_present', 'fmvShow',
       'ogl_setRenderTarget', 'ogl_setPersp', 'ogl_setOrtho', 'vid_setOrtho3D',
       'vid_setViewMatrix', 'ogl_setViewport', 'ogl_setScissor', 'vidInit',
       'init_ogl', 'appGetGame',
       'gGame', 'vid_state', 'vid_state_prev', 'mProj', 'mView', 'mView_packed',
       'mShadow', 'mContacts', 'shaders', 'ogl_textures', 'FBO_custom',
       'FBO_default', 'texDesc', 'app', 'gWidth', 'gHeight', 'gTargetHeight',
       'gTargetWidth']
for n in EXE:
    if n not in h:
        fails.append('%-46s MISSING from Engine.h' % n); checks += 1; continue
    if n not in s:
        fails.append('%-46s MISSING from the PDB' % n); checks += 1; continue
    check(n, h[n], s[n][0])

# XInput globals are spelled with a leading underscore in the PDB.
for hn, pn in (('XInputGetState', '_XInputGetState'),
               ('XInputSetState', '_XInputSetState')):
    check(hn, h.get(hn), s[pn][0] if pn in s else None)

print('  %d symbols checked' % checks)

# ------------------------------------------------------------ struct layouts
print('=== tomb123.exe: struct layouts (src/Engine.h) ===')
size, f = udt('tomb123.exe', 'RenderState')
check('sizeof(RenderState)', 152, size)
for name, off in (('shader', 0), ('depthTest', 4), ('depthWrite', 8),
                  ('depthSlope', 12), ('blend', 16), ('cull', 20),
                  ('tex', 24), ('smp', 40), ('mirrorTex', 56), ('consts', 60),
                  ('mesh', 64), ('proj', 72), ('view', 80), ('shadow', 88),
                  ('model', 96), ('params', 104), ('fogColor', 112),
                  ('joints', 120), ('LPos', 128), ('LCol', 136),
                  ('ambient', 144)):
    check('RenderState::%s' % name, off, f.get(name))

size, f = udt('tomb123.exe', 'Shader')
check('sizeof(Shader)', 60, size)
for name, off in (('id', 0), ('uid', 4), ('cull', 52), ('fvf', 56)):
    check('Shader::%s' % name, off, f.get(name))

size, _ = udt('tomb123.exe', 'mat4')
check('sizeof(mat4)', 64, size)

# shaders[] entry count, as Engine.h::kShaderCount asserts it.
if 'shaders' in s:
    check('kShaderCount', 74, s['shaders'][2] // 60)

# ---------------------------------------------- structural self-check (Engine.cpp)
print('=== structural relationships asserted by Engine.cpp ===')
rel = [('vid_state_prev == vid_state + 160', s['vid_state_prev'][0], s['vid_state'][0] + 160),
       ('mView_packed  == vid_state + 400', s['mView_packed'][0],  s['vid_state'][0] + 400),
       ('mProj + 592   == vid_state',       s['mProj'][0] + 592,   s['vid_state'][0]),
       ('gWidth        == gHeight + 4',     s['gWidth'][0],        s['gHeight'][0] + 4),
       ('gTargetHeight == gHeight + 32',    s['gTargetHeight'][0], s['gHeight'][0] + 32),
       ('ogl_textures + 76 == FBO_default', s['ogl_textures'][0] + 76, s['FBO_default'][0])]
for what, a, b in rel:
    check(what, a, b)

# ------------------------------------------------------- game DLLs (GameDll.cpp)
print('=== tomb1/2/3.dll: globals and layouts (src/GameDll.cpp) ===')
gd = open(os.path.join(ROOT, 'src', 'GameDll.cpp'), encoding='utf-8',
          errors='replace').read()

# One row of GameDllLayout per DLL: module, display name, PE timestamp, then the
# RVAs in declaration order. Parsed positionally (comments stripped first) so
# adding a column to the struct is caught here as a length mismatch rather than
# silently checking the wrong field against the wrong symbol.
LAYOUT = ['lara', 'camera', 'room', 'number_rooms',
          'draw_rooms', 'number_draw_rooms', 'w2v_matrix', 'phd_mxptr',
          'phd_winxmax', 'phd_winymax',
          'outside', 'outside_left', 'outside_right', 'outside_top',
          'outside_bottom',
          'PrintRoomsList', 'S_GetObjectBounds', 'DrawSkyHD',
          'phd_GenerateW2V', 'w2v_scene_return', 'frame_frac', 'lara_item',
          'DrawCreatureHD', 'DrawHair', 'gLaraHead', 'gActorHead', 'objects', 'analogInput',
          'input', 'LaraAboveWater', 'GetCollisionInfo', 'UpdateLaraRoom']

# Not a PDB symbol: the return address FirstPerson.cpp gates on. Checked by
# disassembling the five bytes before it, which must be the E8 rel32 call to
# phd_GenerateW2V inside S_InitialisePolyList -- the same property at runtime
# that makes the gate exact.
DERIVED = {'w2v_scene_return': ('S_InitialisePolyList', 'phd_GenerateW2V')}

def pe_stamp(path):
    with open(path, 'rb') as f:
        d = f.read(4096)
    pe = int.from_bytes(d[0x3C:0x40], 'little')
    return int.from_bytes(d[pe + 8:pe + 12], 'little')

all_rows = []
for m in re.finditer(r'\{\s*L"(tomb[123]\.dll)",\s*"[^"]*",\s*(0x[0-9A-Fa-f]+),(.*?)\}', gd, re.S):
    body = re.sub(r'/\*.*?\*/', '', m.group(3), flags=re.S)
    vals = [int(v, 0) for v in re.findall(r'0x[0-9A-Fa-f]+|\b\d+\b', body)]
    all_rows.append((m.group(1), int(m.group(2), 16), vals))

# The table carries one row per DLL per build. Only the rows for the build in
# PDB\ can be checked against a PDB; the others are checked further down,
# against the images they describe.
rows = [r for r in all_rows if r[1] == pe_stamp(os.path.join(PDB, r[0]))]
if len(rows) != 3:
    fails.append('GameDll.cpp: expected 3 DLL rows for the PDB build, parsed %d' % len(rows))
    checks += 1

for dll, stamp, vals in rows:
    ds = syms(dll)

    if len(vals) != len(LAYOUT):
        fails.append('%-46s %d RVAs in the row, GameDllLayout has %d'
                     % (dll, len(vals), len(LAYOUT)))
        checks += 1
        continue

    for name, got in zip(LAYOUT, vals):
        if name in DERIVED:
            continue          # checked against the image further down
        if name in ds:
            check('%s %s' % (dll, name), got, ds[name][0])
        else:
            # A symbol this build does not have must be spelled 0 in the table,
            # because the code tests for zero to decide whether to touch it.
            # TR1 has no `outside` machinery at all; TR2 and TR3 do.
            check('%s %s absent from the PDB -> 0' % (dll, name), got, 0)

    # draw_rooms is indexed with a hard cap of 200 in PortalCull.cpp, and in
    # tomb1.dll `number_rooms` is the very next global -- so an off-by-one there
    # corrupts the room count rather than overwriting padding.
    check('%s sizeof(draw_rooms) == 400' % dll, ds['draw_rooms'][2], 400)
    check('%s sizeof(w2v_matrix) == 48' % dll, ds['w2v_matrix'][2], 48)

    size, f = udt(dll, 'lara_info')
    check('%s sizeof(lara_info)' % dll, 432, size)
    check('%s lara_info::water_status' % dll, 12, f.get('water_status'))
    check('%s lara_info::turn_rate' % dll, 252, f.get('turn_rate'))
    check('%s lara_info::move_angle' % dll, 254, f.get('move_angle'))
    size, f = udt(dll, 'coll_info')
    check('%s sizeof(coll_info)' % dll, 144, size)
    for fld, want in [('radius',72), ('bad_pos',76), ('bad_neg',80), ('bad_ceiling',84), ('shift',88), ('old',100), ('facing',118), ('coll_type',122), ('trigger',128), ('slopes_are_walls',140)]:
        check('%s coll_info::%s' % (dll,fld), want, f.get(fld))

    size, f = udt(dll, 'camera_info')
    check('%s sizeof(camera_info)' % dll, 128, size)
    check('%s camera_info::pos' % dll, 0, f.get('pos'))

    size, f = udt(dll, 'ANALOG_INPUT_INFO')
    check('%s sizeof(ANALOG_INPUT_INFO)' % dll, 52, size)
    check('%s ANALOG_INPUT_INFO::camTurn' % dll, 4, f.get('camTurn'))

    size, f = udt(dll, 'ROOM_INFO')
    check('%s sizeof(ROOM_INFO)' % dll, 168, size)
    check('%s ROOM_INFO::maxceiling' % dll, 56, f.get('maxceiling'))
    # The fields PortalCull.cpp reads and writes. `door` is the portal list the
    # traversal walks; bound_active bit 0 is "already in draw_rooms"; the four
    # shorts are the clip rect it widens.
    for fld, want in (('door', 8), ('x', 40), ('y', 44), ('z', 48),
                      ('bound_active', 77), ('left', 80), ('right', 82),
                      ('top', 84), ('bottom', 86), ('flags', 102)):
        check('%s ROOM_INFO::%s' % (dll, fld), want, f.get(fld))

    # The layouts FirstPerson.cpp reads: Lara's pose and joints, and the
    # geometry pointer that identifies the face and sunglasses draws.
    size, f = udt(dll, 'ITEM_INFO')
    check('%s ITEM_INFO::mesh_bits' % dll, 12, f.get('mesh_bits'))
    check('%s ITEM_INFO::object_number' % dll, 16, f.get('object_number'))
    check('%s ITEM_INFO::pos' % dll, 88, f.get('pos'))
    check('%s ITEM_INFO::pos_prev' % dll, 108, f.get('pos_prev'))

    size, f = udt(dll, 'object_info')
    check('%s sizeof(object_info)' % dll, 2304, size)
    check('%s object_info::geom' % dll, 88, f.get('geom'))

    size, f = udt(dll, 'GEOM_INFO')
    check('%s sizeof(GEOM_INFO)' % dll, 104, size)
    check('%s GEOM_INFO::mesh' % dll, 16, f.get('mesh'))

    # gLaraHead is the face and the sunglasses; gActorHead the cutscene head.
    # FirstPerson.cpp walks both arrays, so their lengths are load-bearing.
    check('%s gLaraHead is GEOM_INFO[2]' % dll, ds['gLaraHead'][2], 208)
    check('%s gActorHead is GEOM_INFO[3]' % dll, ds['gActorHead'][2], 312)

    size, f = udt(dll, 'game_vector')
    check('%s sizeof(game_vector)' % dll, 16, size)
    check('%s game_vector::y' % dll, 4, f.get('y'))
    check('%s game_vector::room_number' % dll, 12, f.get('room_number'))

# ------------------------------------------------------------ hook prologues
#
# The stolen-byte windows are what make inline patching safe. Three properties
# have to hold, and all three are checked here against the real .text:
#   1. the bytes in the source are the bytes actually at the target;
#   2. the window ends on an instruction boundary;
#   3. no instruction inside the window has a RIP-relative operand, because
#      neither hook site passes displacement fixups.
#
# Two sources install hooks: Hooks.cpp into tomb123.exe, and PortalCull.cpp into
# whichever of tomb1/2/3.dll is running -- so the DLL windows are checked
# against all three images, since one prologue table serves all of them.
print('=== hook prologues (src/Hooks.cpp, src/PortalCull.cpp) ===')
try:
    import pefile
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    def prologues(src):
        """Parse `const uint8_t kNamePrologue[] = {...}` and the stolen counts."""
        txt = open(os.path.join(ROOT, 'src', src), encoding='utf-8',
                   errors='replace').read()
        arrays = {m.group(1): [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', m.group(2))]
                  for m in re.finditer(r'const uint8_t k(\w+)Prologue\[\]\s*=\s*\{([^}]*)\}', txt)}
        stolen = {m.group(2): int(m.group(1))
                  for m in re.finditer(r'(\d+),\s*k(\w+)Prologue,', txt)}
        # `const int kNameRipFixups[] = { 7 };` -- byte offsets of the disp32
        # fields InlineHook rewrites when it copies the window.
        fixups = {m.group(1): sorted(int(x) for x in re.findall(r'\d+', m.group(2)))
                  for m in re.finditer(r'const int k(\w+)RipFixups\[\]\s*=\s*\{([^}]*)\}', txt)}
        return arrays, stolen, fixups

    def check_call_site(image, image_dir, table, tag=''):
        """The 5 bytes before w2v_scene_return must call phd_GenerateW2V."""
        global checks
        ret = table.get('w2v_scene_return')
        target = table.get('phd_GenerateW2V')
        label = '%s%s!w2v_scene_return' % (tag, image)
        if not ret or not target:
            fails.append('%-46s missing from the row' % label); checks += 1; return
        pe = pefile.PE(os.path.join(image_dir, image), fast_load=True)
        data = pe.get_memory_mapped_image()
        site = ret - 5
        if data[site] != 0xE8:
            check('%s is preceded by E8 rel32' % label, hex(data[site]), '0xe8')
            return
        rel = int.from_bytes(data[site + 1:site + 5], 'little', signed=True)
        check('%s calls phd_GenerateW2V' % label, hex(ret + rel), hex(target))

    def check_prologues(image, src, targets, image_dir=PDB, table=None, tag=''):
        global checks
        arrays, stolen, fixups = prologues(src)
        pe = pefile.PE(os.path.join(image_dir, image), fast_load=True)
        data = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        table = syms(image) if table is None else table
        for key, fn in targets.items():
            label = '%s%s!%s' % (tag, image, fn)
            if key not in arrays:
                fails.append('%-46s no kPrologue array in %s' % (label, src))
                checks += 1
                continue
            if fn not in table:
                fails.append('%-46s MISSING from the PDB' % label)
                checks += 1
                continue
            want = bytes(arrays[key])
            rva  = table[fn][0] if isinstance(table[fn], tuple) else table[fn]
            got  = data[rva:rva + len(want)]
            # check(what, <what the source says>, <what the binary says>)
            check('%s prologue bytes' % label, want.hex(), got.hex())

            n = stolen.get(key)
            if n is None:
                fails.append('%-46s no stolen count beside the array' % label)
                checks += 1
                continue
            check('%s stolen >= 5' % label, n >= 5, True)
            check('%s stolen covers the byte pattern' % label, n >= len(want), True)

            # Walk the window: it must end exactly on a boundary, with no RIP
            # operand.
            total, riprel = 0, []
            for ins in md.disasm(data[rva:rva + n + 24], base + rva):
                if total >= n:
                    break
                if 'rip' in ins.op_str:
                    # Where the disp32 field sits inside the stolen window.
                    try:
                        off = total + ins.encoding.disp_offset
                    except AttributeError:
                        off = total + ins.size - 4
                    riprel.append(off)
                total += ins.size
            check('%s window ends on an instruction boundary' % label, n, total)
            # A RIP-relative operand is allowed only where the source declares a
            # fixup for it: InlineHook rewrites exactly those displacements, and
            # one it does not know about would send the trampoline to the wrong
            # address.
            check('%s RIP displacements are all declared as fixups' % label,
                  sorted(riprel), fixups.get(key, []))

    check_prologues('tomb123.exe', 'Hooks.cpp',
                    {'SetPass': 'vid_setPass', 'Validate': 'validate_draw',
                     'Draw': 'ogl_draw', 'Present': 'ogl_present',
                     'FmvShow': 'fmvShow', 'SetRt': 'ogl_setRenderTarget'})

    for dll in ('tomb1.dll', 'tomb2.dll', 'tomb3.dll'):
        check_prologues(dll, 'PortalCull.cpp',
                        {'PrintRoomsList': 'PrintRoomsList',
                         'ObjectBounds': 'S_GetObjectBounds'})
        check_prologues(dll, 'Sky.cpp',
                        {'DrawSkyHD': 'DrawSkyHD'})
        check_prologues(dll, 'FirstPerson.cpp',
                        {'GenerateW2V': 'phd_GenerateW2V',
                         'DrawCreatureHD': 'DrawCreatureHD',
                         'DrawHair': 'DrawHair'})
        row = [r for r in rows if r[0] == dll]
        if row:
            t = dict(zip(LAYOUT, row[0][2]))
            t['phd_GenerateW2V'] = syms(dll)['phd_GenerateW2V'][0]
            check_call_site(dll, PDB, t)

    # ------------------------------------------------ builds without PDBs
    import json
    from capstone.x86 import X86_OP_MEM, X86_REG_RIP

    nopdb_dirs = sys.argv[1:] or [os.path.join(ROOT, n) for n in ('retail', 'gold')
                                  if os.path.isdir(os.path.join(ROOT, n))]

    EXE_LAYOUT = ['vid_setPass', 'validate_draw', 'ogl_draw', 'ogl_present',
                  'fmvShow', 'ogl_setRenderTarget', 'gGame', '_XInputGetState',
                  'vid_state', 'vid_state_prev', 'mProj', 'mView_packed', 'shaders',
                  'ogl_textures', 'FBO_custom', 'FBO_default', 'app', 'gWidth',
                  'gHeight', 'gTargetWidth', 'gTargetHeight']

    # Engine.h rows written as hex literals -- i.e. not the stock row, which is
    # spelled with the rva::/drva:: names checked above.
    eh = open(os.path.join(ROOT, 'src', 'Engine.h'), encoding='utf-8').read()
    exe_rows = {}
    for m in re.finditer(r'constexpr Layout (\w+) = \{(.*?)\};', eh, re.S):
        body = re.sub(r'/\*.*?\*/', '', m.group(2), flags=re.S)
        body = re.sub(r'"[^"]*"', '', body)
        toks = [t.strip() for t in body.split(',') if t.strip()]
        if all(re.fullmatch(r'0x[0-9A-Fa-f]+', t) for t in toks):
            exe_rows[int(toks[0], 16)] = (m.group(1), [int(t, 16) for t in toks[1:]])

    def load(path):
        pe = pefile.PE(path)
        ends = {e.struct.BeginAddress: e.struct.EndAddress
                for e in pe.DIRECTORY_ENTRY_EXCEPTION}
        return pe.get_memory_mapped_image(), pe.OPTIONAL_HEADER.ImageBase, ends

    def insns(img, rva):
        data, base, ends = img
        return list(md.disasm(data[rva:ends[rva]], base + rva))

    def rip_target(ins, base, dest_only=False):
        for op in (ins.operands[:1] if dest_only else ins.operands):
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                return ins.address + ins.size + op.mem.disp - base
        return None

    def app_slots(img, fns, app):
        """APP offset -> function RVA, from `lea rax, [fn]; mov [app+off], rax`."""
        slots = {}
        for f in fns:
            lea = None
            for ins in insns(img, f):
                if ins.mnemonic == 'lea':
                    lea = rip_target(ins, img[1])
                elif ins.mnemonic == 'mov' and lea is not None:
                    t = rip_target(ins, img[1], dest_only=True)
                    if t is not None and app <= t < app + 2800:
                        slots[t - app] = lea
                    lea = None
        return slots

    for d in nopdb_dirs:
        tag = '[%s] ' % os.path.basename(os.path.normpath(d))
        print('=== build without PDBs: %s ===' % d)
        jpath = os.path.join(d, 'port_build.json')
        if not os.path.exists(jpath):
            fails.append('%sno port_build.json -- run tools\\port_build.py %s' % (tag, d))
            checks += 1
            continue
        pj = json.load(open(jpath))
        P = {img: {n: e['new'] for n, e in pj[img]['symbols'].items()} for img in pj}

        # 1. The Engine.h row matches port_build.json.
        stamp = pe_stamp(os.path.join(d, 'tomb123.exe'))
        check(tag + 'tomb123.exe stamp == port_build.json', stamp,
              pj['tomb123.exe']['timestamp'])
        if stamp not in exe_rows:
            fails.append('%sEngine.h has no Layout row for PE 0x%08X' % (tag, stamp))
            checks += 1
            continue
        row_name, vals = exe_rows[stamp]
        check('%s%s field count' % (tag, row_name), len(vals), len(EXE_LAYOUT))
        R = dict(zip(EXE_LAYOUT, vals))
        for n in EXE_LAYOUT:
            check('%s%s %s' % (tag, row_name, n), R.get(n), P['tomb123.exe'][n])

        # 2. The relationships Engine.cpp asserts at runtime, on this row.
        for what, a, b in (
                ('vid_state_prev == vid_state + 160', R['vid_state_prev'], R['vid_state'] + 160),
                ('mView_packed == vid_state + 400',   R['mView_packed'],   R['vid_state'] + 400),
                ('mProj + 592 == vid_state',          R['mProj'] + 592,    R['vid_state']),
                ('gWidth == gHeight + 4',             R['gWidth'],         R['gHeight'] + 4),
                ('gTargetHeight == gHeight + 32',     R['gTargetHeight'],  R['gHeight'] + 32),
                ('ogl_textures + 76 == FBO_default',  R['ogl_textures'] + 76, R['FBO_default'])):
            check(tag + what, a, b)

        # 3. Hook windows, against this image.
        check_prologues('tomb123.exe', 'Hooks.cpp',
                        {'SetPass': 'vid_setPass', 'Validate': 'validate_draw',
                         'Draw': 'ogl_draw', 'Present': 'ogl_present',
                         'FmvShow': 'fmvShow', 'SetRt': 'ogl_setRenderTarget'},
                        image_dir=d, table=R, tag=tag)

        # 4. Independent of the matching: the engine installs every hooked
        #    function into the same APP slot in both builds.
        s_old = syms('tomb123.exe')
        old_img = load(os.path.join(PDB, 'tomb123.exe'))
        new_img = load(os.path.join(d, 'tomb123.exe'))
        installers = ('vidInit', 'init_ogl', 'appInit')
        so = app_slots(old_img, [s_old[n][0] for n in installers], s_old['app'][0])
        sn = app_slots(new_img, [P['tomb123.exe'][n] for n in installers], R['app'])
        for fn in ('vid_setPass', 'ogl_draw', 'ogl_present', 'fmvShow', 'ogl_setRenderTarget'):
            want = sorted(o for o, f in so.items() if f == s_old[fn][0])
            got  = sorted(o for o, f in sn.items() if f == R[fn])
            check('%s%s APP slot' % (tag, fn), got if want else 'no slot in the PDB build', want)

        # 5. validate_draw: same forced mask, same bit tests in the same order,
        #    so ConstBits in Engine.h still holds.
        def bit_tests(img, rva):
            return ['%s %s' % (i.mnemonic, i.op_str) for i in insns(img, rva)
                    if (i.mnemonic == 'mov' and i.op_str.endswith('0x3f001f'))
                    or (i.mnemonic in ('test', 'bt')
                        and re.search(r'\b(bl|ebx), (0x)?[0-9a-f]+$', i.op_str))]
        check(tag + 'validate_draw consts bit tests',
              bit_tests(new_img, R['validate_draw']),
              bit_tests(old_img, s_old['validate_draw'][0]))

        # 6. vid_setViewMatrix writes the same offsets from vid_state. The only
        #    direct evidence for mView_packed, which nothing references
        #    RIP-relatively.
        def view_writes(img, rva, vs):
            out = set()
            for i in insns(img, rva):
                t = rip_target(i, img[1], dest_only=True)
                if i.mnemonic in ('mov', 'movss') and t is not None and abs(t - vs) < 4096:
                    out.add(t - vs)
            return sorted(out)
        vw_new = view_writes(new_img, P['tomb123.exe']['vid_setViewMatrix'], R['vid_state'])
        check(tag + 'vid_setViewMatrix writes relative to vid_state', vw_new,
              view_writes(old_img, s_old['vid_setViewMatrix'][0], s_old['vid_state'][0]))
        check(tag + 'vid_setViewMatrix fills mView_packed[0..11]',
              all(R['mView_packed'] - R['vid_state'] + 4 * k in vw_new for k in range(12)), True)

        # 7. GameDll.cpp rows for these DLLs match port_build.json; their hooks.
        for dll in ('tomb1.dll', 'tomb2.dll', 'tomb3.dll'):
            dstamp = pe_stamp(os.path.join(d, dll))
            mine = [r for r in all_rows if r[0] == dll and r[1] == dstamp]
            if len(mine) != 1:
                fails.append('%sGameDll.cpp has %d rows for %s PE 0x%08X'
                             % (tag, len(mine), dll, dstamp))
                checks += 1
                continue
            check('%s%s row length' % (tag, dll), len(mine[0][2]), len(LAYOUT))
            DR = dict(zip(LAYOUT, mine[0][2]))
            for n in LAYOUT:
                check('%s%s %s' % (tag, dll, n), DR.get(n), P[dll][n])
            check_prologues(dll, 'PortalCull.cpp',
                            {'PrintRoomsList': 'PrintRoomsList',
                             'ObjectBounds': 'S_GetObjectBounds'},
                            image_dir=d, table=DR, tag=tag)
            check_prologues(dll, 'Sky.cpp', {'DrawSkyHD': 'DrawSkyHD'},
                            image_dir=d, table=DR, tag=tag)
            check_prologues(dll, 'FirstPerson.cpp',
                            {'GenerateW2V': 'phd_GenerateW2V',
                             'DrawCreatureHD': 'DrawCreatureHD',
                             'DrawHair': 'DrawHair'},
                            image_dir=d, table=DR, tag=tag)
            check_call_site(dll, d, DR, tag)

except ImportError:
    print('  SKIPPED -- pip install pefile capstone to run this section')

# ---------------------------------------------------------------------- result
print()
if fails:
    print('FAILED -- %d of %d checks disagree with the PDBs:' % (len(fails), checks))
    for f_ in fails:
        print('  ' + f_)
    sys.exit(1)
print('OK -- all %d checks agree with the PDBs.' % checks)
