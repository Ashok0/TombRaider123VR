"""Read-only snapshot of the running game's Lara, room tiles and static bounds."""
import ctypes as C
import json, math, re, struct, sys, time
from pathlib import Path
import pefile, capstone
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tools'))
from port_build import DLL_LAYOUT
pid=int(sys.argv[1]);base=int(sys.argv[2])
game=Path(r'C:\Program Files (x86)\Steam\steamapps\common\Tomb Raider I-III Remastered')
k=C.WinDLL('kernel32',use_last_error=True)
k.OpenProcess.argtypes=[C.c_ulong,C.c_bool,C.c_ulong];k.OpenProcess.restype=C.c_void_p
k.ReadProcessMemory.argtypes=[C.c_void_p,C.c_void_p,C.c_void_p,C.c_size_t,C.c_void_p]
k.CloseHandle.argtypes=[C.c_void_p]
h=k.OpenProcess(0x10,False,pid)
if not h: raise OSError(C.get_last_error())
def read(a,n):
    b=C.create_string_buffer(n)
    if not k.ReadProcessMemory(h,a,b,n,None):raise OSError(C.get_last_error(),hex(a))
    return b.raw
def unpack(a,fmt):return struct.unpack(fmt,read(a,struct.calcsize(fmt)))
def value(a,fmt):return unpack(a,fmt)[0]
try:
    stamp=value(base+value(base+0x3c,'<I')+8,'<I')
    for m in re.finditer(r'\{\s*L"(tomb[123]\.dll)",\s*"[^"]*",\s*(0x[0-9A-Fa-f]+),(.*?)\}',(root/'src/GameDll.cpp').read_text(),re.S):
        if m[1]=='tomb2.dll' and int(m[2],16)==stamp:
            vals=[int(v,0) for v in re.findall(r'0x[0-9A-Fa-f]+|\b\d+\b',re.sub(r'/\*.*?\*/','',m[3],flags=re.S))]
            layout=dict(zip([n for n in DLL_LAYOUT if n!='S_InitialisePolyList'],vals));break
    item=value(base+layout['lara_item'],'<Q');pos=unpack(item+88,'<iiihhh');room_id=value(item+28,'<h')
    rooms=value(base+layout['room'],'<Q');count=value(base+layout['number_rooms'],'<h')
    old=pefile.PE(str(root/'PDB/tomb2.dll')).get_memory_mapped_image()
    disk=pefile.PE(str(game/'2/tomb2.dll')).get_memory_mapped_image()
    signature=old[0xad70:0xad70+28];at=disk.find(signature)
    assert at>=0 and disk.find(signature,at+1)<0
    md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);md.detail=True
    ins=next(md.disasm(disk[at+0x157:at+0x157+7],at+0x157))
    assert ins.mnemonic=='lea' and ins.op_str.startswith('r8, [rip + ')
    statics=base+ins.address+ins.size+ins.operands[1].mem.disp
    # Native LaraAboveWater stores XYZ in the actual global collision record.
    mov=next(md.disasm(disk[layout['LaraAboveWater']+0x10:layout['LaraAboveWater']+0x16],layout['LaraAboveWater']+0x10))
    assert mov.mnemonic=='mov' and mov.operands[0].mem.base==capstone.x86.X86_REG_RIP
    coll=base+mov.address+mov.size+mov.operands[0].mem.disp-100
    result={'position':pos,'room':room_id,'state':value(item+18,'<h'),'input':value(base+layout['input'],'<I'),
        'collision':{'samples':unpack(coll,'<18i'),'radius':value(coll+72,'<i'),'shift':unpack(coll+88,'<3i'),'old':unpack(coll+100,'<3i'),
        'facing_type':unpack(coll+118,'<3h'),'hit_static':value(coll+139,'<B')},'meshes':[],'near_tiles':[]}
    for r in range(count):
        room=rooms+r*168;mesh=value(room+32,'<Q');n=value(room+72,'<h')
        if not 0<=n<=1000:continue
        for i in range(n):
            x,y,z,yaw,shade,shadeb,number=unpack(mesh+i*20,'<iiihhhh')
            if math.hypot(x-pos[0],z-pos[2])>4000 or abs(y-pos[1])>2000:continue
            bounds=unpack(statics+number*28,'<14h')
            result['meshes'].append({'room':r,'index':i,'type':number,'pos':[x,y,z],'yaw':yaw,'bounds':bounds})
        if r!=room_id:continue
        x,y,z=unpack(room+40,'<3i');nx,nz=unpack(room+60,'<2h');floor=value(room+16,'<Q')
        result['room_origin']=[x,y,z];result['room_size']=[nx,nz]
        for ix in range(nz):
            for iz in range(nx):
                tx=x+ix*1024;tz=z+iz*1024
                if abs(tx-pos[0])>2500 or abs(tz-pos[2])>2500:continue
                tile=unpack(floor+(ix*nx+iz)*12,'<4HBbBb')
                result['near_tiles'].append({'xz':[tx,tz],'floor_info':tile})
    if len(sys.argv)>3:
        last=None
        for _ in range(int(sys.argv[4]) if len(sys.argv)>4 else 450):
            row={'pos':unpack(item+88,'<iiihhh'),'state':value(item+18,'<h'),
                'input':value(base+layout['input'],'<I'),'axes':unpack(base+layout['analogInput'],'<4h'),
                'samples':unpack(coll,'<12i'),'shift':unpack(coll+88,'<3i'),
                'facing_type':unpack(coll+118,'<3h'),'hit_static':value(coll+139,'<B')}
            if row!=last:print(json.dumps(row),flush=True);last=row
            time.sleep(.1)
    else:print(json.dumps(result,indent=2))
finally:k.CloseHandle(h)
