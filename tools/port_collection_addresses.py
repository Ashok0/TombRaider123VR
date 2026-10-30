"""Resolve collection-parity symbols without modifying existing build manifests."""
import concurrent.futures
import json
import hashlib
from pathlib import Path
import port_build as port

EXE = ['inputUpdate', 'appProcessMessages', 'isActive', '__security_cookie']
DLL = ['DrawPhaseInventory', 'S_CopyBufferToScreen', 'DoInventoryBackground',
       'DrawRooms', 'PrintRooms', 'S_OutputPolyList', 'S_CmdFlush', 'S_InitialisePolyList',
       'DoFlareInHand', 'DoFlareLight', 'dynamics', 'number_dynamics', 'FlareLightIndex',
       'AddTR2Dynamic', 'TriggerDynamic', 'HD_CollectDynamicLights', 'app']

def resolve(job):
    directory, image = job
    destination = Path('build/collection-parity') / (directory.name + '-' + image + '.json')
    required = EXE if image.endswith('.exe') else DLL
    if destination.exists() and all(n in json.loads(destination.read_text())['symbols'] for n in required):
        return str(destination)
    result = port.resolve(image, str(directory))
    destination.write_text(json.dumps(result, indent=2))
    return str(destination)

if __name__ == '__main__':
    port.EXE_LAYOUT += [n for n in EXE if n not in port.EXE_LAYOUT]
    port.DLL_LAYOUT += [n for n in DLL if n not in port.DLL_LAYOUT]
    jobs = [(Path('build') / directory, image)
            for directory in ['current-retail-verify', 'current-gold-verify',
                              'current-october-verify', 'current-egs-verify']
            for image in port.IMAGES if (Path('build') / directory / image).exists()]
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
        for result in executor.map(resolve, jobs):
            print(result, flush=True)
    evidence = {}
    for directory, image in jobs:
        name = directory.name + '-' + image
        result = json.loads((Path('build/collection-parity') / (name + '.json')).read_text())
        evidence[name] = {
            'timestamp': result['timestamp'],
            'sha256': hashlib.sha256((directory / image).read_bytes()).hexdigest(),
            'symbols': {n: result['symbols'][n] for n in (EXE if image.endswith('.exe') else DLL)
                        if n in result['symbols']},
        }
    Path('tools/collection_addresses.json').write_text(json.dumps(evidence, indent=2) + '\n')
