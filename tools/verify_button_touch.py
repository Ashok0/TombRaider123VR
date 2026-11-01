"""Read-only verification of TR2/3 push-button geometry and native idle poses."""
from pathlib import Path
import re, struct, sys

root=Path(sys.argv[1]);repo=Path(__file__).resolve().parents[1]
profiles=re.findall(r'\{([23]),"([^"]+)",\{([^}]+)\},([^}]+)\}',(repo/'src/ButtonTouch.h').read_text())
assert len(profiles)==14
for game,name,face,back in profiles:
    d=(root/game/'ITEM'/f'{name}.TRM').read_bytes()
    magic,materials=struct.unpack_from('<II',d);assert magic==0x024D5254
    at=8+materials*44;tiles=struct.unpack_from('<I',d,at)[0];at+=4+(tiles+1)//2*4
    bones,indices,n=struct.unpack_from('<III',d,at);assert bones==0
    at+=12+(indices+1)//2*4;assert at+n*24==len(d)
    points=[struct.unpack_from('<3f',d,at+i*24) for i in range(n) if d[at+i*24+16]==1]
    lo=[min(p[k] for p in points) for k in range(3)];hi=[max(p[k] for p in points) for k in range(3)]
    left,right,top,bottom,z=map(lambda x:float(x.rstrip('f')),face.split(','));back=float(back.rstrip('f'))
    assert lo[0]<=left<right<=hi[0] and lo[1]<=top<bottom<=hi[1]
    assert abs(z-lo[2])<.002 and abs(back-hi[2])<.002
print('PASS: 14 measured front/back button faces')
counts={2:0,3:0}
for game in (2,3):
    for path in (root/str(game)/'DATA').rglob('*.PDP'):
        d=path.read_bytes();at=0;t=[]
        for stride in (32,6,8,2,4,2,18):
            n=struct.unpack_from('<I',d,at)[0];at+=4;t.append((at,n));at+=n*stride
        objects=[struct.unpack_from('<IhhIIh',d,t[6][0]+i*18) for i in range(t[6][1])]
        obj=next((o for o in objects if o[0]==(103 if game==2 else 128)),None)
        if not obj: continue
        assert obj[1]==2
        assert struct.unpack_from('<4i',d,t[4][0]+obj[3]*4)==(0,0,0,-23)
        for k in (0,1):
            a=struct.unpack_from('<Ihhii8h',d,t[0][0]+(obj[-1]+k)*32)
            f=struct.unpack_from('<'+str(a[1]>>8)+'h',d,t[5][0]+a[0])
            assert a[2]==1-k and f[6:9]==(1,-567,477)
            assert f[9:]==(16384,16384) if k==0 else f[9:] in ((16384,7,-513),(16384,-32257)),(path,k,f)
        counts[game]+=1
assert all(counts.values())
print('PASS: both authored idle states, root/joint pivots and rotation in installed PDPs:',counts)
