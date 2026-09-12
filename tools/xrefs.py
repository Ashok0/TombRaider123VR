# xrefs.py -- who calls a function, by disassembling every function the PDB names.
#
#   python tools\xrefs.py PDB\tomb123.exe validate_draw ogl_draw
#   python tools\xrefs.py PDB\tomb123.exe            (checks the hook targets)
#
# WHY NOT A LINEAR SWEEP OF .text
#
# The obvious implementation -- disassemble the whole .text section from its
# start -- is wrong and quietly so. x86 is variable-length, .text is peppered
# with alignment padding and jump tables, and a linear sweep desynchronises on
# the first one and then produces plausible-looking garbage for everything after
# it. Run that way this script reported ZERO callers for validate_draw, when in
# fact ogl_draw calls it 1 instruction before glDrawElements.
#
# Disassembling each function separately from its PDB-reported start and size
# cannot desynchronise, because every start is a real instruction boundary.
import sys, os, subprocess
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

HERE = os.path.dirname(os.path.abspath(__file__))

# The six functions Hooks.cpp patches.
DEFAULT_TARGETS = ['vid_setPass', 'validate_draw', 'ogl_draw', 'ogl_present',
                   'fmvShow', 'ogl_setRenderTarget']

def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__ or 'usage: xrefs.py <image> [name ...]')
    img = os.path.abspath(sys.argv[1])
    wanted = sys.argv[2:] or DEFAULT_TARGETS

    out = subprocess.run([sys.executable, os.path.join(HERE, 'pdbdump.py'), img],
                         capture_output=True, text=True).stdout
    funcs, byname = [], {}
    for line in out.splitlines():
        if not line.startswith('0x'):
            continue
        p = line.split(None, 3)
        if len(p) < 4 or p[1] != 'func':
            continue
        try:
            rva = int(p[0], 16)
        except ValueError:
            continue
        size, name = int(p[2]), p[3].strip()
        funcs.append((rva, size, name))
        byname.setdefault(name, rva)

    targets = {}
    for w in wanted:
        if w not in byname:
            print('// not found: %s' % w)
            continue
        targets[byname[w]] = w
    if not targets:
        return 1

    pe = pefile.PE(img, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    data = pe.get_memory_mapped_image()
    md = Cs(CS_ARCH_X86, CS_MODE_64)

    callers = {k: {} for k in targets}
    for rva, size, name in funcs:
        if size <= 0:
            continue
        for ins in md.disasm(data[rva:rva + size], base + rva):
            if ins.mnemonic in ('call', 'jmp') and ins.op_str.startswith('0x'):
                t = int(ins.op_str, 16) - base
                if t in targets and t != rva:
                    callers[t].setdefault(name, 0)
                    callers[t][name] += 1

    print('%d functions disassembled from %s\n' % (len(funcs), os.path.basename(img)))
    for rva, name in sorted(targets.items(), key=lambda kv: kv[1]):
        c = callers[rva]
        if c:
            detail = ', '.join('%s (x%d)' % (n, k) for n, k in sorted(c.items()))
        else:
            # Not "nobody calls it" -- these are installed into the APP struct by
            # vidInit/init_ogl and called through that function pointer by the
            # game DLLs, which is invisible to a static scan of the exe.
            detail = '(no direct call in .text -- reached through the APP vtable)'
        print('  %-20s <- %s' % (name, detail))
    return 0

if __name__ == '__main__':
    sys.exit(main())
