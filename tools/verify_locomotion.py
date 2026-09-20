"""Verify the input frame used by first-person locomotion, including retail DLLs.

Usage: python tools/verify_locomotion.py [game directory]
The S_UpdateInput instruction sequence passes &analogInput to app->inputUpdate.
No adjacency to camera globals is assumed.
"""
import pathlib
import re
import sys
import pefile

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
