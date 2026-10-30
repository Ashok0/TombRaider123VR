"""Verify compression dispatch across supported builds; --generate emits its table.

Usage: python tools/verify_jump_port_binaries.py [fixture-repository] [--generate]
The fixture repository contains PDB and build/current-*-verify directories.
"""
from pathlib import Path
import sys,re,struct,hashlib
import capstone
from capstone.x86 import X86_OP_MEM
import pefile
from port_build import DLL_LAYOUT, Image

root=Path(__file__).resolve().parents[1]
fixture=Path(next((s for s in sys.argv[1:] if not s.startswith('--')),str(root)))
source=(root/'src/GameDll.cpp').read_text(encoding='utf-8')
layouts={}
for m in re.finditer(r'\{\s*L"(tomb[123]\.dll)",\s*"[^"]*",\s*(0x[0-9A-Fa-f]+),(.*?)\}',source,re.S):
    vals=[int(v,0) for v in re.findall(r'0x[0-9A-Fa-f]+|\b\d+\b',re.sub(r'/\*.*?\*/','',m[3],flags=re.S))]
    columns=[n for n in DLL_LAYOUT if n!='S_InitialisePolyList']
    assert len(columns)==len(vals)
    layouts[(m[1],int(m[2],16))]=dict(zip(columns,vals))
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);md.detail=True
rows={};reference={};checks=0
for directory in ['PDB','build/current-retail-verify','build/current-gold-verify','build/current-october-verify','build/current-egs-verify']:
    for game in (1,2,3):
        name=f'tomb{game}.dll';path=fixture/directory/name
        pe=pefile.PE(str(path));data=pe.get_memory_mapped_image();base=pe.OPTIONAL_HEADER.ImageBase
        stamp=pe.FILE_HEADER.TimeDateStamp;layout=layouts[(name,stamp)]
        begin=layout['LaraAboveWater']
        # MSVC splits LaraAboveWater into several unwind regions. Bound this
        # scan to its vicinity and require an exact match to PDB compression,
        # rather than treating the first unwind region as the whole function.
        calls=[i for i in md.disasm(data[begin:begin+2048],base+begin) if i.mnemonic=='call' and
               i.operands[0].type==X86_OP_MEM and i.operands[0].mem.scale==8]
        # Reuse the established porter's operand normalization, retaining all
        # struct offsets and constants while normalizing relocation targets.
        decoder=Image.__new__(Image);decoder.data=data;decoder.base=base
        known,size={1:(0x22210,556),2:(0x3FE10,570),3:(0x5E020,594)}[game]
        if directory=='PDB':reference[game]=[i[1] for i in decoder._tokens(known,known+size)]
        matches=set()
        for call in calls:
            table=call.operands[0].mem.disp
            if table<=0 or table+16*8>len(data):continue
            candidate=struct.unpack_from('<Q',data,table+15*8)[0]-base
            if candidate<0 or candidate+size>len(data):continue
            if [i[1] for i in decoder._tokens(candidate,candidate+size)]==reference[game]:matches.add(candidate)
        assert len(matches)==1,(path,'unique native control[15] compression match',matches)
        address=matches.pop()
        signature=data[address:address+8]
        row=(address,signature)
        if stamp in rows:assert rows[stamp]==row
        rows[stamp]=row;checks+=1
        print(f'{directory}/{name}: dispatch[15]={address:08X}, native compression matches')
text='// Generated and checked by tools/verify_jump_port_binaries.py.\nconstexpr JumpDll kJumpDlls[]={\n'
for stamp,(address,signature) in rows.items():
    text+='    {0x%X,0x%X,{'%(stamp,address)+','.join(f'0x{x:02X}' for x in signature)+'}},\n'
text+='};\n'
target=root/'src/JumpAddresses.inl'
if '--generate' in sys.argv:target.write_text(text,encoding='utf-8')
else:assert target.read_text(encoding='utf-8')==text,'jump address table mismatch'
print(f'{checks} binary dispatch/function checks passed; {len(rows)} unique DLL builds')
