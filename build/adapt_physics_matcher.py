from pathlib import Path
p=Path('tools/port_physics_addresses.py')
s=p.read_text(encoding='utf-8-sig').replace('import json','import difflib\nimport json',1)
a=s.index('        raw = bytearray(ins.bytes)'); b=s.index('    return result',a)
s=s[:a]+'''        ops = []
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
'''+s[b:]
s=s.replace('if f.EndAddress - f.BeginAddress != length:', 'if not length * 0.8 <= f.EndAddress - f.BeginAddress <= length * 1.2:')
s=s.replace('            if tokens(new, f.BeginAddress, f.EndAddress) == expected:\n                matches.append(f.BeginAddress)', '''            ratio = difflib.SequenceMatcher(None, expected, tokens(new, f.BeginAddress, f.EndAddress), autojunk=False).ratio()
            if ratio >= 0.90:
                matches.append((f.BeginAddress, ratio))''')
s=s.replace('rva = matches[0]', 'rva, ratio = matches[0]')
s=s.replace("how='unique normalized whole-function match'", "how=f'unique normalized whole-function match {ratio:.4f}'")
s=s.replace('symbol} {old_rva', 'symbol} score={ratio:.4f} {old_rva')
p.write_text(s)
