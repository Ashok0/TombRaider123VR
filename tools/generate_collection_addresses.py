"""Emit exact-build hook windows, checking instruction boundaries and relocation."""
from pathlib import Path
import json
import hashlib
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP
import port_build as port

ROOT=Path(__file__).resolve().parents[1]
dis=Cs(CS_ARCH_X86,CS_MODE_64);dis.detail=True

def function_instructions(pe,data,rva):
    # MSVC splits one function into multiple chained .pdata entries when its
    # saved registers change. Auditing only the first entry misses its calls.
    entries={f.struct.BeginAddress:f for f in pe.DIRECTORY_ENTRY_EXCEPTION}
    def root(address):
        seen=set()
        while entries[address].unwindinfo.Flags & 4:
            assert address not in seen,'cyclic unwind chain'
            seen.add(address)
            address=entries[address].unwindinfo.FunctionEntry
        return address
    ranges=[(a,f.struct.EndAddress) for a,f in entries.items() if root(a)==rva]
    assert ranges,hex(rva)
    instructions=[i for a,b in sorted(ranges) for i in dis.disasm(data[a:b],a)]
    return instructions,ranges

def hook(data,rva):
    if not rva: return '{}'
    span=0; reloc=[]
    for i in dis.disasm(data[rva:rva+32],rva):
        assert i.mnemonic not in ('call','ret','jmp') and not i.mnemonic.startswith('j'), (hex(rva),i.mnemonic)
        if any(o.type==X86_OP_MEM and o.mem.base==X86_REG_RIP for o in i.operands):
            assert i.disp_size==4
            # InlineHook relocation expects the displacement at the instruction end.
            assert i.disp_offset+4==i.size
            reloc.append(span+i.disp_offset)
        span+=i.size
        if span>=5: break
    assert 5<=span<=32 and len(reloc)<=2
    return '{0x%X,%d,{%s},%d,{%s}}' % (rva,span,','.join(hex(v) for v in data[rva:rva+span]),len(reloc),','.join(map(str,reloc)))

def generate():
    exe=[];dll=[];seen=set()
    evidence=json.loads((ROOT/'tools/collection_addresses.json').read_text())
    for directory in ['PDB','build/current-retail-verify','build/current-gold-verify','build/current-october-verify','build/current-egs-verify']:
        for image in port.IMAGES:
            path=ROOT/directory/image
            if not path.exists(): continue
            pe=pefile.PE(str(path),fast_load=True);data=pe.get_memory_mapped_image();stamp=pe.FILE_HEADER.TimeDateStamp
            key=(image,stamp)
            if key in seen: continue
            seen.add(key)
            if directory=='PDB':
                syms={n:r[0] for n,r in port.old_symbols(image).items()}
            else:
                manifest=evidence[f'{Path(directory).name}-{image}']
                assert manifest['timestamp']==stamp and manifest['sha256']==hashlib.sha256(path.read_bytes()).hexdigest(),(path,'binary differs from address evidence')
                syms={n:r['new'] for n,r in manifest['symbols'].items()}
            def r(n):
                value=syms.get(n,0)
                assert value is not None,(path,n)
                return value
            if image.endswith('.exe'):
                update,messages,active=r('inputUpdate'),r('appProcessMessages'),r('isActive')
                instructions=list(dis.disasm(data[update:update+64],update))
                calls=[i for i in instructions if i.mnemonic=='call' and i.operands[0].imm==messages]
                assert len(calls)==1
                comparisons=[i for i in instructions if i.mnemonic=='cmp' and any(o.type==X86_OP_MEM and o.mem.base==X86_REG_RIP and i.address+i.size+o.mem.disp==active for o in i.operands)]
                assert len(comparisons)==1 and calls[0].address<comparisons[0].address
                gateStart=calls[0].address;gateEnd=comparisons[0].address+comparisons[0].size
                gate=data[gateStart:gateEnd]
                exe.append('{0x%X,%s,%s,0x%X,0x%X,%d,{%s}}' %(stamp,hook(data,update),hook(data,messages),active,gateStart,len(gate),','.join(hex(v) for v in gate)))
            else:
                game=int(image[4]);bg=r('DoInventoryBackground' if game==2 else 'S_CopyBufferToScreen')
                rooms=r('DrawRooms')
                init=r('S_InitialisePolyList');flush=r('S_CmdFlush')
                assert rooms and init and flush and bg,(path,'required scene address')
                assert flush!=r('S_OutputPolyList') and r('app'),(path,'submit-only flush and APP required')
                # Verify the selected submit routine, including its whole PE
                # function extent, cannot present, pace or pump messages.
                pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
                submitted,flushRanges=function_instructions(pe,data,flush)
                indirect=[o.mem.disp for i in submitted if i.mnemonic in ('call','jmp') for o in i.operands if o.type==X86_OP_MEM]
                assert 0x170 in indirect and 0x168 in indirect,(path,'flush must upload and draw')
                assert not any(d in indirect for d in (0x118,0x120,0xA8,0xF8)),(path,'flush must not end the frame')
                assert not any(i.mnemonic=='call' and i.operands[0].type==2 for i in submitted),(path,'audit new flush callees')
                assert all(any(a<=i.operands[0].imm<b for a,b in flushRanges) for i in submitted if i.mnemonic=='jmp' and i.operands[0].type==2),(path,'audit flush tail calls')
                initialized,_=function_instructions(pe,data,init)
                callbacks=[o.mem.disp for i in initialized if i.mnemonic in ('call','jmp') for o in i.operands if o.type==X86_OP_MEM]
                assert all(d in callbacks for d in (0x138,0x140,0x150,0x158,0x160,0x1E0)),(path,'native clear/camera/viewport restoration ABI')
                if game!=1: assert r('DoFlareLight') and r('dynamics') and r('number_dynamics'),(path,'required flare address')
                inventory=r('DrawPhaseInventory')
                # The background follows initialization in the native inventory phase.
                calls=[i for i in dis.disasm(data[inventory:inventory+1600],inventory) if i.mnemonic=='call' and i.operands[0].type==2]
                assert any(a.address<b.address for a in calls for b in calls if a.operands[0].imm==init and b.operands[0].imm==bg),(path,'inventory ordering')
                dll.append('{%d,0x%X,%s,%s,%s,0x%X,0x%X,0x%X,0x%X,0x%X}' %(game,stamp,hook(data,bg),hook(data,init),hook(data,r('DoFlareLight')),rooms,flush,r('dynamics'),r('number_dynamics'),r('app')))
    text='// Generated by tools/generate_collection_addresses.py; exact-build windows.\n'
    text+='constexpr ExeBuild exeBuilds[]={\n'+',\n'.join(exe)+'\n};\n'
    text+='constexpr DllBuild dllBuilds[]={\n'+',\n'.join(dll)+'\n};\n'
    return text

if __name__=='__main__':
    import sys
    text=generate();output=ROOT/'src/CollectionAddresses.inl'
    if '--check' in sys.argv:
        assert output.read_text()==text,'CollectionAddresses.inl differs from verified binaries'
        print('PASS: collection hook windows, focus gates, submit-only flush and menu restoration ABI')
    else:
        output.write_text(text);print(output)
