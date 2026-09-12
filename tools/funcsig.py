# funcsig.py -- print C prototypes for functions in a PE+PDB.
# Usage: python funcsig.py <image> <name> [name ...]
import ctypes as C, ctypes.wintypes as W, sys, os
dbg = C.WinDLL('dbghelp.dll'); MAX=2000
class SI(C.Structure):
    _fields_=[('SizeOfStruct',C.c_ulong),('TypeIndex',C.c_ulong),('Reserved',C.c_ulonglong*2),
              ('Index',C.c_ulong),('Size',C.c_ulong),('ModBase',C.c_ulonglong),('Flags',C.c_ulong),
              ('Value',C.c_ulonglong),('Address',C.c_ulonglong),('Register',C.c_ulong),
              ('Scope',C.c_ulong),('Tag',C.c_ulong),('NameLen',C.c_ulong),('MaxNameLen',C.c_ulong),
              ('Name',C.c_char*(MAX+1))]
CB=C.WINFUNCTYPE(C.c_bool,C.POINTER(SI),C.c_ulong,C.c_void_p)
dbg.SymSetOptions(0x2|0x40)
dbg.SymLoadModuleExW.argtypes=[W.HANDLE,W.HANDLE,C.c_wchar_p,C.c_wchar_p,C.c_ulonglong,C.c_ulong,C.c_void_p,C.c_ulong]
dbg.SymLoadModuleExW.restype=C.c_ulonglong
dbg.SymGetTypeInfo.argtypes=[W.HANDLE,C.c_ulonglong,C.c_ulong,C.c_int,C.c_void_p]
h=W.HANDLE(0x1234); dbg.SymInitialize(h,None,False)
base=dbg.SymLoadModuleExW(h,None,os.path.abspath(sys.argv[1]),None,0x10000000,0,None,0)
def ti(t,w,ct=C.c_ulong):
    v=ct()
    return v.value if dbg.SymGetTypeInfo(h,base,t,w,C.byref(v)) else None
def nm(t):
    p=C.c_void_p()
    if not dbg.SymGetTypeInfo(h,base,t,1,C.byref(p)) or not p.value: return None
    s=C.wstring_at(p); C.windll.kernel32.LocalFree(p); return s
BT={1:'void',2:'char',3:'wchar_t',6:'int',7:'unsigned',8:'float',10:'bool',13:'long',14:'ulong',31:'HRESULT'}
def ts(t,d=0):
    if t is None or d>6: return '?'
    tag=ti(t,0); ln=ti(t,2,C.c_ulonglong)
    if tag==16:
        b=ti(t,5); n=BT.get(b,'bt%s'%b)
        if b in(6,7): n={1:'int8',2:'int16',4:'int32',8:'int64'}.get(ln,n); n=('u'+n) if b==7 else n
        if b==8 and ln==8: n='double'
        return n
    if tag==14: return ts(ti(t,4),d+1)+'*'
    if tag==15: return '%s[%s]'%(ts(ti(t,4),d+1),ti(t,12))
    if tag in(11,12,17): return nm(t) or 'tag%d'%tag
    return nm(t) or 'tag%s'%tag
def kids(t):
    n=ti(t,13) or 0
    if not n: return []
    class FC(C.Structure): _fields_=[('Count',C.c_ulong),('Start',C.c_ulong),('ChildId',C.c_ulong*n)]
    fc=FC(n,0)
    return list(fc.ChildId) if dbg.SymGetTypeInfo(h,base,t,7,C.byref(fc)) else []
def sig(ftid):
    ret=ts(ti(ftid,4)); args=[ts(ti(c,4)) for c in kids(ftid) if ti(c,0)==17 or True]
    return ret, args
want=[a.lower() for a in sys.argv[2:]]; res={}
def cb(p,s,c):
    x=p.contents
    n=x.Name[:x.NameLen].decode('u8','replace') if x.NameLen else x.Name.decode('u8','replace')
    if x.Tag==5 and n.lower() in want: res[n]=(x.Address-base,x.Size,x.TypeIndex)
    return True
dbg.SymEnumSymbols.argtypes=[W.HANDLE,C.c_ulonglong,C.c_char_p,CB,C.c_void_p]
dbg.SymEnumSymbols(h,base,b'*',CB(cb),None)
for n,(rva,sz,tid) in sorted(res.items(),key=lambda x:x[1][0]):
    ft=ti(tid,4) if ti(tid,0)!=13 else tid
    r,a=sig(tid if ti(tid,0)==13 else tid)
    print('0x%08X  %5d  %s %s(%s);'%(rva,sz,r,n,', '.join(a) if a else 'void'))
for w in sys.argv[2:]:
    if w not in res: print('// not found: %s'%w)
