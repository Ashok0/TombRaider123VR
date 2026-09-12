# disasm.py -- disassemble a function by name from a PE, annotating RIP-relative
# operands and call targets with PDB symbol names.
# Usage: python disasm.py <image> <func-name> [max-bytes]
import sys, os, subprocess, bisect
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

img = os.path.abspath(sys.argv[1])
target = sys.argv[2]
here = os.path.dirname(os.path.abspath(__file__))

# symbol table from pdbdump
syms = []   # (rva, name, kind)
out = subprocess.run([sys.executable, os.path.join(here, 'pdbdump.py'), img],
                     capture_output=True, text=True).stdout
for line in out.splitlines():
    if not line.startswith('0x'): continue
    p = line.split(None, 3)
    if len(p) < 4: continue
    try:
        r = int(p[0], 16)
    except ValueError:
        continue
    syms.append((r, p[3].strip(), p[1]))
syms.sort()
addrs = [s[0] for s in syms]
byname = {}
for rva, name, kind in syms:
    byname.setdefault(name, (rva, kind))

def sym_at(rva):
    i = bisect.bisect_right(addrs, rva) - 1
    if i < 0: return None
    base, name, kind = syms[i]
    if rva == base: return name
    if rva - base < 0x4000: return '%s+0x%X' % (name, rva - base)
    return None

if target not in byname:
    cands = [n for n in byname if target.lower() in n.lower()][:20]
    raise SystemExit('no symbol %r; candidates: %s' % (target, cands))
frva, _ = byname[target]

pe = pefile.PE(img, fast_load=True)
imgbase = pe.OPTIONAL_HEADER.ImageBase
data = pe.get_memory_mapped_image()
size = int(sys.argv[3]) if len(sys.argv) > 3 else None
if size is None:
    # function size from the pdb dump line
    for rva, name, kind in syms:
        if name == target and kind == 'func':
            for line in out.splitlines():
                p = line.split(None, 3)
                if len(p) >= 4 and p[3].strip() == target and p[1] == 'func':
                    size = int(p[2]); break
            break
size = size or 512

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
code = data[frva:frva + size]
print('// %s  rva=0x%08X  size=%d' % (target, frva, size))
for ins in md.disasm(code, imgbase + frva):
    rva = ins.address - imgbase
    note = ''
    # RIP-relative data reference
    if 'rip' in ins.op_str:
        try:
            disp = ins.disp
            tgt = rva + ins.size + disp
            s = sym_at(tgt)
            if s: note = '   ; %s  [rva 0x%08X]' % (s, tgt)
        except Exception: pass
    if ins.mnemonic in ('call', 'jmp') and ins.op_str.startswith('0x'):
        tgt = int(ins.op_str, 16) - imgbase
        s = sym_at(tgt)
        if s: note = '   ; -> %s' % s
    print('  %08X  %-8s %-40s%s' % (rva, ins.mnemonic, ins.op_str, note))
