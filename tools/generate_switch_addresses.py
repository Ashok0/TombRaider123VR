"""Generate the lever table from PDB symbols and reviewed port manifests."""
from pathlib import Path
import json
import pefile
from port_build import old_symbols
from port_switch_addresses import NAMES

root=Path(__file__).resolve().parents[1]
rows={}
for directory in ['PDB','build/current-retail-verify','build/current-gold-verify','build/current-october-verify']:
    manifest=None if directory=='PDB' else json.loads((root/directory/'port_build.json').read_text())
    for game in (1,2,3):
        image=f'tomb{game}.dll'
        pe=pefile.PE(str(root/directory/image),fast_load=True)
        symbols=old_symbols(image) if manifest is None else manifest[image]['symbols']
        addresses=[symbols[n][0] if manifest is None else symbols[n]['new'] for n in NAMES]
        row=[pe.FILE_HEADER.TimeDateStamp,*addresses]
        if row[0] in rows: assert rows[row[0]]==row
        rows[row[0]]=row
(root/'src/SwitchAddresses.inl').write_text('// PDB symbols and port_build.json; checked by verify_physical_switch_binaries.py.\nconstexpr SwitchDll kSwitchDlls[]={\n'+''.join('    {'+','.join(f'0x{x:X}' for x in row)+'},\n' for row in rows.values())+'};\n')
