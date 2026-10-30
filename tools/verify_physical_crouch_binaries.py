"""Read-only TR3 crouch verification; never loads or executes game DLL code.

Usage: python tools/verify_physical_crouch_binaries.py [fixture-repository]
Compare native crouch entry, Duck and all-fours handlers across supported DLLs.
Operand normalization preserves bit masks, struct offsets and branch structure.
"""
from pathlib import Path
import re, struct, subprocess, sys
import capstone, pefile
from capstone.x86 import X86_OP_IMM, X86_OP_MEM
from port_build import DLL_LAYOUT, Image

root=Path(__file__).resolve().parents[1]
fixture=Path(sys.argv[1]) if len(sys.argv)>1 else root
symbols={}
dump=subprocess.check_output([sys.executable,str(root/'tools/pdbdump.py'),str(fixture/'PDB/tomb3.dll')],text=True)
for line in dump.splitlines():
    parts=line.split(None,3)
    if len(parts)==4 and parts[0].startswith('0x') and parts[1]=='func':
        symbols[parts[3]]=(int(parts[0],16),int(parts[2]))
layouts={}
source=(root/'src/GameDll.cpp').read_text(encoding='utf-8')
for m in re.finditer(r'\{\s*L"(tomb[123]\.dll)",\s*"[^"]*",\s*(0x[0-9A-Fa-f]+),(.*?)\}',source,re.S):
    if m[1]!='tomb3.dll':continue
    values=[int(v,0) for v in re.findall(r'0x[0-9A-Fa-f]+|\b\d+\b',re.sub(r'/\*.*?\*/','',m[3],flags=re.S))]
    columns=[n for n in DLL_LAYOUT if n!='S_InitialisePolyList']
    assert len(values)==len(columns)
    layouts[int(m[2],16)]=dict(zip(columns,values))
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);md.detail=True
reference={};checks=0
for directory in ('PDB','build/current-retail-verify','build/current-gold-verify','build/current-october-verify','build/current-egs-verify'):
    path=fixture/directory/'tomb3.dll'
    pe=pefile.PE(str(path));data=pe.get_memory_mapped_image();base=pe.OPTIONAL_HEADER.ImageBase
    layout=layouts[pe.FILE_HEADER.TimeDateStamp]
    decoder=Image.__new__(Image);decoder.data=data;decoder.base=base
    def tokens(address,size):return [i[1] for i in decoder._tokens(address,address+size)]
    if directory=='PDB':
        for name in ('lara_col_duck','lara_col_all4s','ModernControlsStartDuck'):
            reference[name]=tokens(*symbols[name])
    begin=layout['LaraAboveWater']
    tables={i.operands[0].mem.disp for i in md.disasm(data[begin:begin+2048],base+begin)
        if i.mnemonic=='call' and i.operands[0].type==X86_OP_MEM and i.operands[0].mem.scale==8}
    tables={t for t in tables if 0<t and t+81*8<len(data)}
    # These are the complete functions that read the toggle, check standing
    # clearance and choose Duck/Stand. The unrelated as_duck handler changes
    # instruction scheduling between builds; its full bytes are not required.
    for name,state in (('lara_col_duck',71),('lara_col_all4s',80)):
        size=symbols[name][1]
        matches=set()
        for table in tables:
            addr=struct.unpack_from('<Q',data,table+state*8)[0]-base
            if 0<=addr<len(data)-size and tokens(addr,size)==reference[name]:matches.add(addr)
        assert len(matches)==1,(path,name,'native handler differs',matches)
        checks+=1
    # StartDuck is called from the standing handler, not directly dispatched.
    matches=set();size=symbols['ModernControlsStartDuck'][1]
    for table in tables:
        stop=struct.unpack_from('<Q',data,table+2*8)[0]-base
        if not 0<=stop<len(data)-symbols['lara_as_stop'][1]:continue
        for ins in md.disasm(data[stop:stop+symbols['lara_as_stop'][1]],base+stop):
            if ins.mnemonic!='call' or ins.operands[0].type!=X86_OP_IMM:continue
            addr=ins.operands[0].imm-base
            if 0<=addr<len(data)-size and tokens(addr,size)==reference['ModernControlsStartDuck']:matches.add(addr)
    assert len(matches)==1,(path,'StartDuck differs',matches)
    checks+=1
    print(f'{directory}/tomb3.dll: native entry, Duck and all-fours handlers match')
print(f'{checks} read-only native function comparisons passed.')
