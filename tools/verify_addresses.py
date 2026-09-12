# verify_addresses.py -- prove src/Engine.h and src/GameDll.cpp agree with the PDBs.
#
# Every constant in those files was produced by pdbdump.py/typedump.py. This
# re-derives them from the PDBs and diffs, so a typo, a stale edit, or a game
# patch is caught here rather than by a crash in the game.
#
#   python tools\verify_addresses.py
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
          'PrintRoomsList', 'S_GetObjectBounds', 'DrawSkyHD']

rows = []
for m in re.finditer(r'\{\s*L"(tomb[123]\.dll)",\s*"[^"]*",\s*(0x[0-9A-Fa-f]+),(.*?)\}', gd, re.S):
    body = re.sub(r'/\*.*?\*/', '', m.group(3), flags=re.S)
    vals = [int(v, 0) for v in re.findall(r'0x[0-9A-Fa-f]+|\b\d+\b', body)]
    rows.append((m.group(1), m.group(2), vals))

if len(rows) != 3:
    fails.append('GameDll.cpp: expected 3 DLL rows, parsed %d' % len(rows))
    checks += 1

for dll, stamp, vals in rows:
    ds = syms(dll)

    if len(vals) != len(LAYOUT):
        fails.append('%-46s %d RVAs in the row, GameDllLayout has %d'
                     % (dll, len(vals), len(LAYOUT)))
        checks += 1
        continue

    for name, got in zip(LAYOUT, vals):
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

    size, f = udt(dll, 'camera_info')
    check('%s sizeof(camera_info)' % dll, 128, size)
    check('%s camera_info::pos' % dll, 0, f.get('pos'))

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
        return arrays, stolen

    def check_prologues(image, src, targets):
        global checks
        arrays, stolen = prologues(src)
        pe = pefile.PE(os.path.join(PDB, image), fast_load=True)
        data = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        table = syms(image)
        for key, fn in targets.items():
            label = '%s!%s' % (image, fn)
            if key not in arrays:
                fails.append('%-46s no kPrologue array in %s' % (label, src))
                checks += 1
                continue
            if fn not in table:
                fails.append('%-46s MISSING from the PDB' % label)
                checks += 1
                continue
            want = bytes(arrays[key])
            rva  = table[fn][0]
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
                    riprel.append('%s %s' % (ins.mnemonic, ins.op_str))
                total += ins.size
            check('%s window ends on an instruction boundary' % label, n, total)
            check('%s window is free of RIP-relative operands' % label,
                  riprel if riprel else 'none', 'none')

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
