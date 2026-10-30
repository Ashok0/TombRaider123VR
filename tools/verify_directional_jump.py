"""Run native TR1/2/3 compression/rotation on private fixture memory.

No game process is touched. Floor queries use an accept/reject fixture; CRT
atan2/fmod use Python math callbacks. Jump dispatch and rotation are native.
"""
from pathlib import Path
import ctypes as C
import math
import struct
import subprocess
import sys
import pefile

root = Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).resolve().parents[1]
kernel = C.WinDLL('kernel32', use_last_error=True)
kernel.VirtualAlloc.argtypes = [C.c_void_p, C.c_size_t, C.c_uint32, C.c_uint32]
kernel.VirtualAlloc.restype = C.c_void_p
kernel.VirtualFree.argtypes = [C.c_void_p, C.c_size_t, C.c_uint32]
kernel.FlushInstructionCache.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t]
clear = True
floor_angles = []

@C.CFUNCTYPE(C.c_int16, C.c_void_p, C.c_int16)
def floor_front(item, angle):
    floor_angles.append(angle)
    return 0 if clear else -1024

@C.CFUNCTYPE(C.c_double, C.c_double, C.c_double)
def atan2(y, x):
    return math.atan2(y, x)

@C.CFUNCTYPE(C.c_double, C.c_double, C.c_double)
def fmod(x, y):
    return math.fmod(x, y)

def signed(angle):
    return C.c_int16(angle).value

for game in (1, 2, 3):
    path = root / f'PDB/tomb{game}.dll'
    output = subprocess.check_output([sys.executable, str(root/'tools/pdbdump.py'), str(path)], text=True)
    symbols = {}
    for line in output.splitlines():
        fields = line.split(None, 3)
        if len(fields) == 4 and fields[0].startswith('0x'):
            symbols[fields[3]] = int(fields[0], 16)
    pe = pefile.PE(str(path));data = pe.get_memory_mapped_image()
    image = kernel.VirtualAlloc(None, pe.OPTIONAL_HEADER.SizeOfImage, 0x3000, 0x40)
    assert image
    try:
        C.memmove(image, data, len(data))
        for name, callback in [('LaraFloorFront',floor_front), ('atan2',atan2), ('fmod',fmod)]:
            patch = b'\x48\xb8'+struct.pack('<Q',C.cast(callback,C.c_void_p).value)+b'\xff\xe0'
            C.memmove(image+symbols[name],patch,len(patch))
        kernel.FlushInstructionCache(C.c_void_p(-1),image,len(data))
        app=C.create_string_buffer(4096);item=C.create_string_buffer(3664)
        game_query=C.CFUNCTYPE(C.c_int)(lambda:game-1)
        C.c_void_p.from_buffer(app,24).value=C.cast(game_query,C.c_void_p).value
        C.c_void_p.from_address(image+symbols['app']).value=C.addressof(app)
        C.c_uint32.from_buffer(app,0x854).value=2
        native=C.CFUNCTYPE(None,C.c_void_p,C.c_void_p)(image+symbols['lara_as_compress'])
        def field(name, kind=C.c_int16, offset=0):
            return kind.from_address(image+symbols[name]+offset)
        failures_reproduced=0;checks=0
        for status in (0,2,4):
            for yaw in (0,7300,-28000):
                for direction,axes,goal,turn in ((4,(-32767,0),27,-16384),(8,(32767,0),26,16384),(2,(0,-32767),25,32768)):
                    for clear in (False,True):
                        for corrected in (False,True):
                            C.memset(item,0,len(item));C.memset(image+symbols['lara'],0,432)
                            field('lara',offset=2).value=status;field('lara',offset=4).value=1
                            C.c_int16.from_buffer(item,0x12).value=15
                            C.c_int16.from_buffer(item,0x14).value=15
                            C.c_int16.from_buffer(item,0x66).value=yaw
                            field('input',C.c_uint32).value=direction|0x10
                            # Same scope as the FP hook: only unarmed takeoff
                            # loses analog rotation; the requested bits remain.
                            muted=corrected and status==0
                            field('analogInput').value=0 if muted else axes[0]
                            field('analogInput',offset=2).value=0 if muted else axes[1]
                            field('analogInput',offset=4).value=yaw
                            field('camera',C.c_int32,0x20).value=0
                            floor_angles.clear();native(item,None)
                            actual_yaw=C.c_int16.from_buffer(item,0x66).value
                            actual_goal=C.c_int16.from_buffer(item,0x14).value
                            if not corrected and status==0:
                                assert actual_yaw!=yaw, (game,'missing reproduction')
                                failures_reproduced+=1
                            else:
                                assert actual_yaw==yaw, (game,status,yaw,actual_yaw)
                                assert actual_goal==(goal if clear else 15)
                                assert floor_angles==[signed(yaw+turn)]
                                if clear: assert field('lara',offset=0xfe).value==signed(yaw+turn)
                            checks+=1
        # STOP calls this BEFORE compression. A backward armed stick injects
        # Left/Right, which the native jump dispatcher prioritizes over Back.
        gun_rotation=C.CFUNCTYPE(None,C.c_void_p)(image+symbols['ModernControlsGunRotation'])
        armed_failures=0
        for status in (2,4):
            for yaw in (0,7300,-28000):
                for corrected in (False,True):
                    C.memset(item,0,len(item));C.memset(image+symbols['lara'],0,432)
                    field('lara',offset=2).value=status;field('lara',offset=4).value=1
                    C.c_int16.from_buffer(item,0x12).value=2
                    C.c_int16.from_buffer(item,0x14).value=15
                    C.c_int16.from_buffer(item,0x66).value=yaw
                    field('input',C.c_uint32).value=2|0x10|0x80
                    field('analogInput').value=0
                    field('analogInput',offset=2).value=0 if corrected else -32767
                    field('analogInput',offset=4).value=yaw
                    field('camera',C.c_int32,0x20).value=0
                    gun_rotation(item)
                    clear=True;floor_angles.clear();native(item,None)
                    goal=C.c_int16.from_buffer(item,0x14).value
                    if corrected:
                        assert goal==25 and floor_angles==[signed(yaw+32768)], (game,status,yaw,goal,floor_angles)
                        assert field('lara',offset=0xfe).value==signed(yaw+32768)
                    else:
                        assert goal in (26,27), (game,status,yaw,goal)
                        armed_failures+=1
                    checks+=1
        print(f'TR{game}: {checks} native cases passed; reproduced {failures_reproduced} unarmed rotations and {armed_failures} armed back-to-side misdirections; corrected direction and clearance verified',flush=True)
    finally:
        kernel.VirtualFree(image,0,0x8000)
