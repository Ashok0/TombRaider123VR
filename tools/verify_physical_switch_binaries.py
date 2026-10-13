"""Check lever RVAs, hook bytes, native call chains and ordinary object slots.

Usage: python tools/verify_physical_switch_binaries.py [game/fixture directories...]
Always checks PDB images. Reads binaries without modifying them.
"""
from pathlib import Path
import re
import sys
from functools import lru_cache
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

root=Path(__file__).resolve().parents[1]
rows={}
for values in re.findall(r'\{(0x[0-9A-F]+(?:,0x[0-9A-F]+){13})\}',(root/'src/SwitchAddresses.inl').read_text()):
    row=[int(n,16) for n in values.split(',')]
    assert row[0] not in rows
    rows[row[0]]=row
assert len(rows)==9
md=Cs(CS_ARCH_X86,CS_MODE_64);md.detail=True
for directory in [root/'PDB',*map(Path,sys.argv[1:])]:
    for game in (1,2,3):
        path=directory/f'tomb{game}.dll'
        pe=pefile.PE(str(path))
        stamp,collision,test,move,bounds,get_bounds,items,count,animate,calc,changes,ranges,commands,control=rows[pe.FILE_HEADER.TimeDateStamp]
        signatures={collision:'48 89 5c 24 10',test:'48 89 6c 24 '+('10' if game==3 else '20'),
            move:'48 89 5c 24 '+('08' if game==1 else '10'),
            animate:{1:'40 53 41 54 48 83 ec 38',2:'40 53 48 83 ec 40 48 89',
                     3:'40 53 41 '+('54' if stamp==0x6A4B490D else '55')+' 48 83 ec 48'}[game],
            calc:{1:'48 89 4c 24 08 55 41 55',2:'48 89 6c 24 20 56 41 54',3:'48 89 4c 24 08 53 55 56'}[game]}
        extents={e.struct.BeginAddress:e.struct.EndAddress for e in pe.DIRECTORY_ENTRY_EXCEPTION}
        # MSVC splits functions into contiguous unwind regions (AnimateItem has five).
        for address in extents:
            end=extents[address]
            while end in extents: end=extents[end]
            extents[address]=end
        def code(address):
            assert address in extents,(path,hex(address),'function boundary')
            return list(md.disasm(pe.get_data(address,extents[address]-address),address))
        @lru_cache(None)
        def refs(address):
            instructions=code(address)
            memory={i.address+i.size+o.mem.disp for i in instructions for o in i.operands
                    if o.type==X86_OP_MEM and o.mem.base==X86_REG_RIP}
            calls={i.operands[0].imm for i in instructions if i.mnemonic in ('call','jmp') and i.operands[0].type==X86_OP_IMM}
            return memory,calls
        for address,signature in signatures.items():
            expected=bytes.fromhex(signature)
            assert pe.get_data(address,len(expected))==expected,(path,hex(address),'signature')
            code(address)
        memory,calls=refs(collision)
        assert {test,move,animate}<=calls,(path,'collision calls')
        assert {items,bounds}<=memory,(path,'collision data')
        memory,calls=refs(animate)
        assert commands in memory,(path,'commands')
        assert any({changes,ranges}<=refs(call)[0] for call in calls if call in extents),(path,'GetChange tables')
        assert animate in refs(control)[1] and items in refs(control)[0],(path,'native switch control')
        assert get_bounds in extents
        # ObjectObjects installs the same handler/control at the wall-lever slot.
        import json
        if directory==root/'PDB':
            from port_build import old_symbols
            objects=old_symbols(path.name)['objects'][0]
        else:
            # Existing supported layout, independent of the new lever address table.
            manifest=directory/'port_build.json'
            if not manifest.exists():
                manifest=root/'build/current-october-verify/port_build.json'
            objects=json.loads(manifest.read_text())[path.name]['symbols']['objects']['new']
        slot={1:55,2:104,3:129}[game]
        registration=objects+slot*2304
        # ObjectObjects is a leaf in TR1, with no unwind-table entry. Locate its
        # actual LEA/store sequences instead of assuming all functions have pdata.
        data=pe.get_memory_mapped_image()
        import struct
        def registration_store(target,register,slot):
            signature=bytes((0x48,0x8d,register));at=0
            while True:
                at=data.find(signature,at)
                if at<0: return False
                if at+7+struct.unpack_from('<i',data,at+3)[0]==target:
                    for ins in md.disasm(data[at:at+512],at):
                        if ins.mnemonic=='mov' and ins.operands[0].type==X86_OP_MEM:
                            mem=ins.operands[0].mem
                            if mem.base==X86_REG_RIP and ins.address+ins.size+mem.disp==slot: return True
                at+=1
        assert registration_store(collision,0x05,registration+64),(path,'ordinary collision registration')
        assert registration_store(control,0x0d,registration+24),(path,'ordinary control registration')
        # Count comes from native references, not a guessed adjacency to another global.
        assert any(count in refs(address)[0] for address in extents),(path,'level_items reference')
        print(f'PASS: {path}: hook signatures, native calls, transition/command tables and wall-lever slot {slot}')
