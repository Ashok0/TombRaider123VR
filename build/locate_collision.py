import sys,pathlib,difflib
sys.path.insert(0,'tools')
from port_build import Image,old_symbols
root=pathlib.Path('C:/Program Files (x86)/Steam/steamapps/common/Tomb Raider I-III Remastered')
for n in ('tomb1.dll','tomb2.dll','tomb3.dll'):
 s=old_symbols(n); a=Image('PDB/'+n); p=root/n
 if not p.exists(): p=root/n[4]/n
 b=Image(str(p))
 for name in ('GetCollisionInfo','UpdateLaraRoom'):
  addr=s[name][0]; seq=[i[1] for i in a.funcs[addr]['ins']]; candidates=[]
  for r,f in b.funcs.items():
   q=[i[1] for i in f['ins']]
   if abs(len(q)-len(seq))>max(20,len(seq)*.15): continue
   ratio=difflib.SequenceMatcher(None,seq,q,autojunk=False).ratio()
   if ratio>.85: candidates.append((ratio,r))
  print(n,name,hex(addr),[(x,hex(r)) for x,r in sorted(candidates,reverse=True)],flush=True)
