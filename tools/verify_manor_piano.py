"""Verify the Manor collision mesh and replay native piano collision on private memory.

Usage: python tools/verify_manor_piano.py <installed-game-directory>
Does not attach to, write to or call code in a running game.
"""
from pathlib import Path
import ctypes as C
import re, struct, subprocess, sys
import pefile

root=Path(__file__).resolve().parents[1]
d=(Path(sys.argv[1])/'2/DATA/ASSAULT.TR2').read_bytes()
assert struct.unpack_from('<I',d)[0]==45
a=1800+struct.unpack_from('<I',d,1796)[0]*196608+4
rooms=struct.unpack_from('<H',d,a)[0];a+=2
assert rooms==99
for room in range(rooms):
    origin=struct.unpack_from('<4i',d,a);a+=16
    n=struct.unpack_from('<I',d,a)[0];a+=4+n*2
    n=struct.unpack_from('<H',d,a)[0];a+=2+n*32
    nz,nx=struct.unpack_from('<2H',d,a);a+=4+nz*nx*8+6
    n=struct.unpack_from('<H',d,a)[0];a+=2+n*24
    n=struct.unpack_from('<H',d,a)[0];a+=2
    if room==8:
        assert origin==(34816,75776,0,-2816) and (nz,nx,n)==(11,14,2)
        instances=d[a:a+n*20]
        assert struct.unpack_from('<3i4h',instances)==(45568,0,83456,-16384,4096,4096,13)
    a+=n*20+4
tables={}
for name,size in [('floors',2),('meshes',2),('meshptrs',4),('anims',32),('states',6),
                  ('ranges',8),('commands',2),('trees',4),('frames',2),('objects',18),('statics',32)]:
    n=struct.unpack_from('<I',d,a)[0];a+=4;tables[name]=d[a:a+n*size];a+=n*size
b=struct.unpack_from('<I',tables['meshptrs'],169*4)[0]
n=struct.unpack_from('<h',tables['meshes'],b+10)[0];assert n==64
vertices=[struct.unpack_from('<3h',tables['meshes'],b+12+i*6) for i in range(n)]
source=(root/'src/ManorPianoMesh.inl').read_text().split('ManorPianoVertices[]={',1)[1].split('};',1)[0]
assert vertices==[tuple(map(int,m)) for m in re.findall(r'\{(-?\d+),(-?\d+),(-?\d+)\}',source)]
source=(root/'src/ManorPianoCollision.h').read_text().split('ManorPianoParts[]={',1)[1].split('};',1)[0]
parts=[]
for count,indices in re.findall(r'\{(\d+),\{([^}]+)\}\}',source):
    part=list(map(int,indices.split(',')));assert len(part)==int(count) and len(part)<=12
    parts.append(set(part))
assert set.union(*parts)==set(range(64))
# Every rendered face belongs to at least one convex component, so splitting
# the mesh cannot silently omit a solid body panel, leg, bench or lid face.
o=b+12+n*6
normals=struct.unpack_from('<h',tables['meshes'],o)[0];o+=2
if normals>0:o+=normals*6
else:o+=-normals*2
quads=struct.unpack_from('<h',tables['meshes'],o)[0];o+=2
assert quads==47
for i in range(quads):
    face=set(struct.unpack_from('<4h',tables['meshes'],o+i*10))
    assert any(face<=part for part in parts),face
print('ASSAULT.TR2: scene fingerprint, all 64 mesh vertices and coverage of all 47 faces verified',flush=True)

symbols={}
output=subprocess.run([sys.executable,str(root/'tools/pdbdump.py'),str(root/'PDB/tomb2.dll')],capture_output=True,text=True,check=True).stdout
for line in output.splitlines():
    f=line.split(None,3)
    if len(f)==4 and f[0].startswith('0x'):symbols[f[3]]=int(f[0],16)
pe=pefile.PE(str(root/'PDB/tomb2.dll'));data=pe.get_memory_mapped_image()
k=C.WinDLL('kernel32',use_last_error=True)
k.VirtualAlloc.argtypes=[C.c_void_p,C.c_size_t,C.c_uint32,C.c_uint32];k.VirtualAlloc.restype=C.c_void_p
k.VirtualFree.argtypes=[C.c_void_p,C.c_size_t,C.c_uint32]
k.FlushInstructionCache.argtypes=[C.c_void_p,C.c_void_p,C.c_size_t]
image=k.VirtualAlloc(None,pe.OPTIONAL_HEADER.SizeOfImage,0x3000,0x40)
assert image
try:
    C.memmove(image,data,len(data))
    # This fixture has one room; suppress portal discovery, retaining the
    # actual native static intersection/response routine unchanged.
    C.memmove(image+symbols['GetNewRoom'],b'\xc3',1)
    k.FlushInstructionCache(C.c_void_p(-1),image,len(data))
    roomdata=C.create_string_buffer(99*168);meshes=C.create_string_buffer(instances)
    C.c_void_p.from_address(image+symbols['room']).value=C.addressof(roomdata)
    C.c_void_p.from_buffer(roomdata,8*168+32).value=C.addressof(meshes)
    C.c_int16.from_buffer(roomdata,8*168+72).value=2
    for at in range(0,len(tables['statics']),32):
        number,mesh,*bounds,flags=struct.unpack_from('<I14h',tables['statics'],at)
        C.memmove(image+symbols['static_objects']+number*28,struct.pack('<14h',mesh,flags,*bounds),28)
    native=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_int32,C.c_int32,C.c_int32,C.c_int16,C.c_int32)(image+symbols['CollideStaticObjects'])
    def probe(x,z,oldx,oldz,exclude):
        coll=C.create_string_buffer(144)
        C.c_int32.from_buffer(coll,72).value=100
        for off,v in [(100,oldx),(104,0),(108,oldz)]:C.c_int32.from_buffer(coll,off).value=v
        C.c_int16.from_buffer(coll,118).value=12135;C.c_int16.from_buffer(coll,120).value=1
        C.c_int32.from_buffer(meshes,4).value=32768 if exclude else 0
        native(coll,x,0,z,8,762)
        C.c_int32.from_buffer(meshes,4).value=0
        return C.c_uint8.from_buffer(coll,139).value
    assert probe(44563,83779,44520,83761,False)==1
    assert probe(44563,83779,44520,83761,True)==0
    # The adjacent static at (44544,81408) still participates normally.
    assert probe(44544,81408,44000,81408,True)==1
    assert bytes(meshes)[:40]==instances
    print('Native TR2 replay: phantom obstruction reproduced; per-instance exclusion clears it and retains the adjacent static',flush=True)
finally:k.VirtualFree(image,0,0x8000)
