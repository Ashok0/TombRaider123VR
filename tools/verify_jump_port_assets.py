"""Read-only verification of the takeoff/landing port against every gameplay PDP.
Usage: python tools/verify_jump_port_assets.py <installed-game-directory>
"""
from pathlib import Path
import struct,sys
from verify_jump_priority import verify as verify_priority

root=Path(sys.argv[1])
for game in (1,2,3):
    banks=checks=0
    for path in (root/str(game)).rglob('*.PDP'):
        if not verify_priority(path):continue
        data=path.read_bytes();cursor=0;tables={}
        for name,size in [('anims',32),('changes',6),('ranges',8),('commands',2)]:
            count,=struct.unpack_from('<I',data,cursor);cursor+=4
            tables[name]=data[cursor:cursor+count*size];cursor+=count*size
        def anim(n):return struct.unpack_from('<hii8h',tables['anims'],n*32+6)
        def check(condition,detail):
            global checks
            checks+=1
            assert condition,(path,detail)
        for clip,state,next_clip,frames in [(16,3,17,7),(18,3,19,7),(74,25,75,3),
                (76,3,77,3),(78,27,79,3),(80,26,81,3),(91,28,28,3)]:
            a=anim(clip);b=anim(next_clip)
            check(a[0]==state and a[4]-a[3]+1==frames and a[5]==next_clip and a[6]==b[3] and b[0]==state,
                  (clip,'native takeoff transition'))
            check(a[9]==(game+2 if frames==7 else 1 if game==1 else 2),(clip,'command count'))
            command=struct.unpack_from('<3h',tables['commands'],a[10]*2)
            check(command==(2,-110,2) if clip==91 else command==(2,-100,75 if frames==7 else 50),
                  (clip,'original native takeoff velocity'))
        a=anim(73)
        check(a[0]==15 and a[4]-a[3]==10 and a[5]==91 and a[9]==(2 if game==1 else 3),'compression metadata')
        for clip,goal in [(0,1),(103,2)]:
            a=anim(clip)
            check(a[0]==goal and a[4]>a[3]>=0 and a[5]==clip and a[2]==0 and
                  a[1]==(47<<16 if goal==1 else 0) and a[9]==(game*2 if goal==1 else 0),
                  (clip,'safe landing destination'))
        for clip in [31,82,92,99]:
            check(anim(clip)[0]==(1 if clip==92 else 58 if game==1 and clip==99 else 2),(clip,'soft landing/water exit'))
        banks+=1
    assert banks
    print(f'TR{game}: {banks} gameplay banks, {checks} takeoff/landing checks passed')
