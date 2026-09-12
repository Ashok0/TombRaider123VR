# prologue.py -- print the first instructions of named functions with raw bytes,
# flagging RIP-relative operands (which must not be blindly stolen by a hook).
import sys, os, subprocess, bisect
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
img=os.path.abspath(sys.argv[1]); here=os.path.dirname(os.path.abspath(__file__))
out=subprocess.run([sys.executable,os.path.join(here,'pdbdump.py'),img],capture_output=True,text=True).stdout
by={}
for line in out.splitlines():
    if not line.startswith('0x'): continue
    p=line.split(None,3)
    if len(p)<4: continue
    try: r=int(p[0],16)
    except ValueError: continue
    by.setdefault(p[3].strip(),(r,p[1]))
pe=pefile.PE(img,fast_load=True); base=pe.OPTIONAL_HEADER.ImageBase; data=pe.get_memory_mapped_image()
md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True
for name in sys.argv[2:]:
    if name not in by: print('// not found: %s'%name); continue
    rva,_=by[name]
    print('%s  rva=0x%08X' % (name, rva))
    total=0
    for ins in md.disasm(data[rva:rva+40], base+rva):
        raw=' '.join('%02X'%b for b in ins.bytes)
        rip='  <-- RIP-RELATIVE' if 'rip' in ins.op_str else ''
        total+=ins.size
        print('   +%-2d %-26s %-8s %-32s%s' % (total-ins.size, raw, ins.mnemonic, ins.op_str, rip))
        print('        (cumulative stolen = %d)' % total)
        if total>=16: break
    print()
