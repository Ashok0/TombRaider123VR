"""Resolve physical lever symbols using the existing PDB-to-retail matcher."""
import json
from pathlib import Path
import sys
import port_build as port

NAMES = ['SwitchCollision', 'TestLaraPosition', 'MoveLaraPosition', 'Switch1Bounds',
         'GetBoundsAccurate', 'items', 'level_items', 'AnimateItem', 'CalcItemMatrices',
         'changes', 'ranges', 'commands', 'SwitchControl']

if __name__ == '__main__':
    port.DLL_LAYOUT += [n for n in NAMES if n not in port.DLL_LAYOUT]
    for directory in map(Path, sys.argv[1:]):
        path = directory / 'port_build.json'
        manifest = json.loads(path.read_text())
        for game in (1, 2, 3):
            image = f'tomb{game}.dll'
            result = port.resolve(image, str(directory))
            for name in NAMES:
                entry = result['symbols'][name]
                assert entry['new'], (image, name, entry)
                manifest[image]['symbols'][name] = entry
                print(directory.name, image, name, hex(entry['new']), entry['how'], flush=True)
        path.write_text(json.dumps(manifest, indent=1) + '\n')
