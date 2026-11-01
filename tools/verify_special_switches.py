"""Read-only installed small-switch/airlock mesh, animation and native-registration checks."""
from pathlib import Path
import json,re,struct,sys
import pefile
from capstone import Cs,CS_ARCH_X86,CS_MODE_64
from capstone.x86 import X86_OP_MEM,X86_REG_RIP
from port_build import old_symbols

repo=Path(__file__).resolve().parents[1];root=Path(sys.argv[1])
profiles=re.findall(r'\{([23]),(\d+),(\d+),(\d+),"([^"]+)",(true|false),([^}]+)\}',(repo/'src/SpecialSwitch.inl').read_text())
assert len(profiles)==9
for game,slot,joints,joint,name,wheel,values in profiles:
    x,y,z,radius=[float(v.rstrip('f')) for v in values.split(',')]
    d=(root/game/'ITEM'/f'{name}.TRM').read_bytes();magic,materials=struct.unpack_from('<II',d);assert magic==0x024d5254
    at=8+materials*44;tiles=struct.unpack_from('<I',d,at)[0];at+=4+(tiles+1)//2*4
    bones,indices,n=struct.unpack_from('<III',d,at);assert bones==0;at+=12+(indices+1)//2*4
    assert at+n*24==len(d)
    points=[struct.unpack_from('<3f',d,at+i*24) for i in range(n) if d[at+i*24+16]==int(joint)]
    lo=[min(p[k] for p in points) for k in range(3)];hi=[max(p[k] for p in points) for k in range(3)]
    assert lo[1]<=y<=hi[1] and lo[2]<=z<=hi[2],(name,lo,hi)
    if wheel=='true':
        rim=[p for p in points if abs(((p[0]-x)**2+(p[1]-y)**2)**.5-radius)<20 and abs(p[2]-z)<20]
        assert len(rim)>=16,(name,len(rim))
    else: assert lo[0]<=-x<=x<=hi[0],name
print('PASS: nine measured small-switch handles and airlock rims')
counts={2:0,3:0}
for game in (2,3):
    for path in (root/str(game)/'DATA').rglob('*.PDP'):
        d=path.read_bytes();at=0;t=[]
        for stride in (32,6,8,2,4,2,18):
            n=struct.unpack_from('<I',d,at)[0];at+=4;t.append((at,n));at+=n*stride
        for i in range(t[6][1]):
            o=struct.unpack_from('<IhhIIh',d,t[6][0]+i*18)
            if o[0] not in ((92,93) if game==2 else (117,118)):continue
            wheel=o[0]==(92 if game==2 else 117);assert o[1]==(1 if wheel else 2)
            anims=[struct.unpack_from('<Ihhii8h',d,t[0][0]+(o[-1]+k)*32) for k in range(3 if wheel and game==2 else 4)]
            assert [a[2] for a in anims]==([0,1,1] if wheel and game==2 else [0,1,1,0] if wheel else [1,0,0,1])
            for a in anims:
                assert not a[3] and not a[4]
                cmd=a[-1]
                for c in range(a[-2]):
                    op=struct.unpack_from('<h',d,t[3][0]+cmd*2)[0];cmd+=1
                    assert op in (4,5),(path,o[0],op)
                    if op==5:cmd+=2
            if wheel and game==2:assert anims[1][6]-anims[1][5]==140 and anims[1][7]==o[-1]+2
            counts[game]+=1
print('PASS: installed special-object animation tables:',counts)

rows={int(v.split(',')[0],16):[int(n,16) for n in v.split(',')] for v in re.findall(r'\{(0x[0-9A-F]+(?:,0x[0-9A-F]+){13})\}',(repo/'src/SwitchAddresses.inl').read_text())}
md=Cs(CS_ARCH_X86,CS_MODE_64);md.detail=True
for folder in [repo/'PDB',repo/'build/current-retail-verify',repo/'build/current-october-verify']:
    for game in (2,3):
        name=f'tomb{game}.dll';pe=pefile.PE(str(folder/name));data=pe.get_memory_mapped_image();row=rows[pe.FILE_HEADER.TimeDateStamp]
        objects=old_symbols(name)['objects'][0] if folder.name=='PDB' else json.loads((folder/'port_build.json').read_text())[name]['symbols']['objects']['new']
        for slot in ((92,93) if game==2 else (117,118)):
            for target,reg,field in [(row[1],5,64),(row[-1],13,24)]:
                signature=bytes((0x48,0x8d,reg));at=0;found=False
                while True:
                    at=data.find(signature,at)
                    if at<0:break
                    if at+7+struct.unpack_from('<i',data,at+3)[0]==target:
                        for ins in md.disasm(data[at:at+2048],at):
                            if ins.mnemonic=='mov' and ins.operands[0].type==X86_OP_MEM:
                                m=ins.operands[0].mem
                                if m.base==X86_REG_RIP and ins.address+ins.size+m.disp==objects+slot*2304+field:found=True;break
                    if found:break
                    at+=1
                assert found,(folder,name,slot,field)
        print('PASS: small-switch/airlock native collision and control registration',folder.name,game)
