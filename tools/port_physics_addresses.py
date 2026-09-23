"""Extend existing port manifests with uniquely matched chest-physics hooks."""
import difflib
import json
import pathlib
import sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_GRP_JUMP, CS_GRP_CALL
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP
ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGETS = {'tomb123.exe': ('shader_init', 0xEC10), 'tomb1.dll': ('DrawLaraHD', 0x6D9A0), 'tomb2.dll': ('DrawLaraHD', 0x9FC80), 'tomb3.dll': ('DrawLaraHD', 0xE9FA0)}
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

def tokens(pe, start, end):
    result = []
    for ins in md.disasm(pe.get_data(start, end - start), start):
        ops = []
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                ops.append('m[rip]')
            elif op.type == X86_OP_MEM:
                ops.append(f'm[{op.mem.base},{op.mem.index},{op.mem.scale},{op.mem.disp}]')
            elif op.type == X86_OP_IMM and (ins.group(1) or ins.group(2) or ins.group(7)):
                ops.append('T')
            elif op.type == X86_OP_IMM:
                ops.append(f'i{op.imm}')
            else:
                ops.append(f'r{op.reg}')
        result.append(ins.mnemonic + ' ' + ','.join(ops))
    return result

def extend(directory):
    directory = pathlib.Path(directory)
    path = directory / 'port_build.json'
    manifest = json.loads(path.read_text())
    for image, (symbol, old_rva) in TARGETS.items():
        old = pefile.PE(str(ROOT / 'PDB' / image))
        new = pefile.PE(str(directory / image))
        entry = next(e.struct for e in old.DIRECTORY_ENTRY_EXCEPTION if e.struct.BeginAddress == old_rva)
        expected = tokens(old, old_rva, entry.EndAddress)
        matches = []
        length = entry.EndAddress - old_rva
        for e in new.DIRECTORY_ENTRY_EXCEPTION:
            f = e.struct
            if not length * 0.8 <= f.EndAddress - f.BeginAddress <= length * 1.2:
                continue
            ratio = difflib.SequenceMatcher(None, expected, tokens(new, f.BeginAddress, f.EndAddress), autojunk=False).ratio()
            if ratio >= 0.90:
                matches.append((f.BeginAddress, ratio))
        if len(matches) != 1:
            raise RuntimeError(f'{image} {symbol}: expected unique match, got {matches}')
        rva, ratio = matches[0]
        manifest[image]['symbols'][symbol] = dict(old=old_rva, new=rva, how=f'unique normalized whole-function match {ratio:.4f}')
        print(f'{directory.name}/{image}: {symbol} score={ratio:.4f} {old_rva:#x} -> {rva:#x}; prologue {new.get_data(rva, 16).hex(" ")}')
        old.close()
        new.close()
    path.write_text(json.dumps(manifest, indent=1) + '\n')

if __name__ == '__main__':
    for directory in sys.argv[1:]:
        extend(directory)
