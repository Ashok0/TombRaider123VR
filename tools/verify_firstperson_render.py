"""Verify IK-related rendering globals in stock, retail and Gold binaries.
Usage: python tools/verify_firstperson_render.py [ported-build-directory ...]
The PDB identifies stock symbols. Native instruction operands independently
confirm the ported render-pass global and every copied hand-descriptor field.
"""
import json
import sys
from pathlib import Path
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP
from port_build import old_symbols

root=Path(__file__).resolve().parent.parent
md=Cs(CS_ARCH_X86,CS_MODE_64);md.detail=True
for directory in [root/'PDB', *map(Path,sys.argv[1:])]:
    manifest=None if directory.name=='PDB' else json.loads((directory/'port_build.json').read_text())
    for game in (1,2,3):
        name=f'tomb{game}.dll'
        symbols=old_symbols(name)
        def address(name):
            return symbols[name][0] if manifest is None else manifest[f'tomb{game}.dll']['symbols'][name]['new']
        pe=pefile.PE(str(directory/name));data=pe.get_memory_mapped_image();base=pe.OPTIONAL_HEADER.ImageBase
        def instructions(fn):
            start=address(fn)
            # The MSVC unwind table splits DrawLaraHD into several fragments;
            # the first .pdata entry does not cover its hand draws. Use the
            # complete PDB function extent for this operand search.
            end=start+symbols[fn][2]
            return list(md.disasm(data[start:end],base+start))
        ins=instructions('DrawLaraHD')
        # DrawLaraHD addresses gLaraHand through image-base + index*104,
        # not a RIP-relative pointer. A match of the first field alone is weak:
        # require all seven loads that copy the complete 104-byte descriptor.
        offsets=set()
        render_refs=0
        for fn in ('DrawLaraHD','DrawCreatureHD','DrawHair'):
            for i in instructions(fn):
                for op in i.operands:
                    if op.type!=X86_OP_MEM:continue
                    if op.mem.base==X86_REG_RIP:
                        render_refs+=i.address-base+i.size+op.mem.disp==address('gRenderPass')
                    elif fn=='DrawLaraHD' and i.mnemonic in ('movups','movsd'):
                        if op.mem.disp>=address('gLaraHand'):offsets.add(op.mem.disp-address('gLaraHand'))
        assert set(range(0,97,16))<=offsets,(directory,name,'hand descriptor addresses',offsets)
        assert render_refs,(directory,name,'no render-pass reference')
        print(f'{directory.name} {name}: native hand table and shadow-pass global verified')
