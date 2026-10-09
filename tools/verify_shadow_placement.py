"""Audit the native shadow-before-scene contract in all supported TR1/2/3 DLLs.
Run from the repository root: python -B tools/verify_shadow_placement.py [ported-build-directory ...]
No game files are modified. Generic address checks separately verify every
production hook window ends on a complete instruction and needs no RIP fixup.
"""
import json,re,struct,sys
from pathlib import Path
import pefile
from capstone import Cs,CS_ARCH_X86,CS_MODE_64
from capstone.x86 import X86_OP_MEM,X86_OP_IMM,X86_REG_RIP
from port_build import old_symbols

md=Cs(CS_ARCH_X86,CS_MODE_64);md.detail=True
source=Path('src/FirstPerson.cpp').read_text()
window=re.search(r'kDrawToShadowPrologue\[\]\s*=\s*\{([^}]+)\}',source).group(1)
window=bytes(int(x,16) for x in re.findall(r'0x([0-9A-Fa-f]+)',window))
assert 'reinterpret_cast<void*>(&Detour_DrawToShadow),9,' in source
assert len(window)==9
hooks=Path('src/Hooks.cpp').read_text()
guard=hooks.index('if (FirstPersonShadowPass())',hooks.index('void __cdecl Detour_validate_draw()'))
assert hooks.index('} physicsDrawGuard;')<guard<hooks.index('const bool inject =',guard)
assert 'g_hValidateDraw.Original<Fn_validate_draw>()();\n        return;' in hooks[guard:guard+180]
for game in (1,2,3):
    name=f'tomb{game}.dll';symbols=old_symbols(name)
    for directory in map(Path,['PDB', *(sys.argv[1:] or ['build/current-retail-verify','build/current-gold-verify'])]):
        manifest=None if directory.name=='PDB' else json.loads((directory/'port_build.json').read_text())[name]['symbols']
        def address(key):return symbols[key][0] if manifest is None else manifest[key]['new']
        pe=pefile.PE(str(directory/name));data=pe.get_memory_mapped_image();base=pe.OPTIONAL_HEADER.ImageBase
        start=address('DrawToShadow')
        assert data[start:start+len(window)]==window,(directory,name,'production hook bytes')
        # Caller: DrawToShadow(room); S_InitialisePolyList(0). Resolve independently
        # from calls to the pre-existing scene initializer, not from our new RVA.
        callers=[]
        for m in re.finditer(b'\xe8',data):
            at=m.start()
            if at<7 or at+5>len(data):continue
            if at+5+struct.unpack_from('<i',data,at+1)[0]!=address('S_InitialisePolyList'):continue
            if data[at-7]==0xe8 and data[at-2:at]==b'\x33\xc9':
                callers.append(at-2+struct.unpack_from('<i',data,at-6)[0])
        assert callers and set(callers)=={start},(directory,name,'shadow-before-scene call order',callers)
        # MSVC splits unwind records. Scan the complete reference-sized body,
        # with a small allowance for retail instruction scheduling differences.
        ins=list(md.disasm(data[start:start+symbols['DrawToShadow'][2]+128],base+start))
        setter=[];pass_writes=[];interpolations=[]
        for i in ins:
            if i.mnemonic=='call' and i.operands and i.operands[0].type==X86_OP_MEM:
                if i.operands[0].mem.disp==0x1f0:setter.append(i.address)
            if i.mnemonic=='mov' and len(i.operands)==2:
                dst,src=i.operands
                if dst.type==X86_OP_MEM and dst.mem.base==X86_REG_RIP and src.type==X86_OP_IMM:
                    if i.address-base+i.size+dst.mem.disp==address('gRenderPass') and src.imm==4:pass_writes.append(i.address)
            if i.mnemonic=='call' and i.operands[0].type==X86_OP_IMM:
                if manifest is None and i.operands[0].imm-base==symbols['InterpolatePos'][0]:interpolations.append(i.address)
        assert len(setter)==1 and len(pass_writes)==1 and pass_writes[0]<setter[0],(directory,name,'native receiver upload')
        if manifest is None:assert len(interpolations)==1 and interpolations[0]<setter[0],(name,'interpolated native camera origin')
        print(f'{directory/name}: production hook, native light pass, receiver upload and shadow-before-scene order verified')
