import sys,pathlib,difflib,re,pefile
sys.path.insert(0,'tools')
from port_build import Image,old_symbols
root=pathlib.Path('C:/Program Files (x86)/Steam/steamapps/common/Tomb Raider I-III Remastered')
for n in ('tomb1.dll','tomb2.dll','tomb3.dll'):
 s=old_symbols(n); a=Image('PDB/'+n); p=root/n
 if not p.exists(): p=root/n[4]/n
 b=Image(str(p)); addr=s['LaraAboveWater'][0]; seq=[i[1] for i in a.funcs[addr]['ins']]
 candidates=[]
 for r,f in b.funcs.items():
  q=[i[1] for i in f['ins']]
  if abs(len(q)-len(seq))>30: continue
  ratio=difflib.SequenceMatcher(None,seq,q,autojunk=False).ratio()
  if ratio>.8: candidates.append((ratio,r))
 print(n,'stock',hex(addr),bytes(a.data[addr:addr+5]).hex(),'matches',[(x,hex(r),bytes(b.data[r:r+5]).hex()) for x,r in sorted(candidates,reverse=True)])
 for label,img in [('stock',a),('retail',b)]:
  hits=list(re.finditer(rb'\x48\x83\xec\x28\x48\x8b\x05....\x48\x8d\x0d....\x8b\x15....\xff\x50\x60',img.data,re.S)); assert len(hits)==1
  lo=hits[0].start(); ins=img._tokens(lo,lo+45)
  print(label, 'input candidates', [(hex(i[0]),i[1],hex(i[3]) if i[3] else '') for i in ins if i[3]])
