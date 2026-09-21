"""Verify the input frame used by first-person locomotion, including retail DLLs.

Usage: python tools/verify_locomotion.py [game directory]
The S_UpdateInput instruction sequence passes &analogInput to app->inputUpdate.
No adjacency to camera globals is assumed.
"""
import pathlib
import re
import sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

ROOT = pathlib.Path(__file__).resolve().parent.parent
PATTERN = rb"\x48\x83\xec\x28\x48\x8b\x05....\x48\x8d\x0d....\x8b\x15....\xff\x50\x60"
source = (ROOT / "src/GameDll.cpp").read_text(encoding="utf-8")
for directory in [ROOT / "PDB"] + [pathlib.Path(p) for p in sys.argv[1:]]:
    for name in ("tomb1.dll", "tomb2.dll", "tomb3.dll"):
        path = directory / name
        if not path.exists():
            path = directory / name[4] / name
        pe = pefile.PE(str(path))
        data = pe.get_memory_mapped_image()
        matches = list(re.finditer(PATTERN, data, re.S))
        assert len(matches) == 1, (name, "ambiguous inputUpdate call")
        match = matches[0]
        address = match.start() + 18 + int.from_bytes(match[0][14:18], "little", signed=True)
        row = re.search(r'\{\s*L"' + name + r'",[^\{]+?0x%08X,(.*?)\}' % pe.FILE_HEADER.TimeDateStamp,
                        source, re.S)
        assert row, (name, "missing build")
        field = re.search(r'/\* analogInput\s*\*/\s*(0x[0-9A-Fa-f]+)', row[1])
        print(f"{directory.name}/{name}: analogInput=0x{address:08X}")
        if field:
            assert int(field[1], 16) == address, (name, "wrong analogInput RVA")
        else:
            raise SystemExit("Missing analogInput field")
        fields = dict((key, int(value, 16)) for key, value in re.findall(
            r'/\*\s*(\w+)\s*\*/\s*(0x[0-9A-Fa-f]+)', row[1]))
        # The input action word is the MOV [RIP+disp32],EAX after app->inputGet.
        start = match.start()
        assert data[start+34:start+36] == b'\x89\x05'
        decoded_input = start + 40 + int.from_bytes(data[start+36:start+40], 'little', signed=True)
        assert fields['input'] == decoded_input, (name, 'input action word')
        above = fields['LaraAboveWater']
        expected = bytes.fromhex('48 89 5c 24') + bytes([8 * int(name[4])])
        assert data[above:above+5] == expected, (name, 'movement hook prologue')
        animate = fields['AnimateLara']
        if name == 'tomb1.dll':
            animation_prologue = bytes.fromhex('48 89 7c 24 20')
        elif name == 'tomb2.dll':
            animation_prologue = bytes.fromhex(
                '40 57 41 54 41 57' if data[animate] == 0x40 else '57 41 54 41 57')
        else:
            animation_prologue = bytes.fromhex(
                '40 57 41 54 41 55' if data[animate] == 0x40 else '57 41 54 41 55')
        assert data[animate:animate+len(animation_prologue)] == animation_prologue, \
            (name, 'AnimateLara hook prologue')
        # Validate native helper targets by CALL instructions in their callers,
        # independently of table adjacency or a guessed build-wide delta.
        md = Cs(CS_ARCH_X86, CS_MODE_64)
        funcs = {e.struct.BeginAddress: e.struct.EndAddress for e in pe.DIRECTORY_ENTRY_EXCEPTION}
        def calls_at(rva):
            return [int(i.op_str, 16) - pe.OPTIONAL_HEADER.ImageBase
                    for i in md.disasm(data[rva:funcs[rva]], pe.OPTIONAL_HEADER.ImageBase+rva)
                    if i.mnemonic == 'call' and i.op_str.startswith('0x')]
        # TR2/3 split this routine into multiple unwind (.pdata) chunks.
        # Check the distinctive UpdateLaraRoom(item, -381) call sequence
        # across the contiguous body rather than stopping at the first chunk.
        room_calls = list(re.finditer(rb'\xba\x83\xfe\xff\xff\x48\x8b[\xcb\xcd]\xe8....',
                                      data[above:above+1024], re.S))
        assert len(room_calls) == 1, (name, 'room-update call sequence')
        call = above + room_calls[0].start() + 8
        target = call + 5 + int.from_bytes(data[call+1:call+5], 'little', signed=True)
        assert fields['UpdateLaraRoom'] == target, (name, 'native room update call')
        # Match lara_col_stop's constant height/room argument setup and direct
        # call to GetCollisionInfo. This pattern is unique per supported DLL.
        collision_calls = []
        for rva, end in funcs.items():
            # The stop routine is small; all builds pass height 762 on stack.
            body = data[rva:end]
            if len(body) > 1500 or b'\xfa\x02\x00\x00' not in body:
                continue
            if fields['GetCollisionInfo'] in calls_at(rva):
                collision_calls.append(rva)
        assert collision_calls, (name, 'no standing-height collision caller')
        print(f"  input=0x{decoded_input:08X} simulation=0x{above:08X} "
              f"animation=0x{animate:08X} collision/room calls verified")
