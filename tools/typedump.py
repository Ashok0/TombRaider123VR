# typedump.py -- dump UDT layouts from a PE + PDB via dbghelp type info.
# Usage: python typedump.py <image.exe|dll> [TypeName ...]   (no names = list all UDTs)
import ctypes as C, ctypes.wintypes as W, sys, os

dbg = C.WinDLL('dbghelp.dll')
MAX_SYM_NAME = 2000

class SYMBOL_INFO(C.Structure):
    _fields_ = [('SizeOfStruct', C.c_ulong), ('TypeIndex', C.c_ulong),
                ('Reserved', C.c_ulonglong*2), ('Index', C.c_ulong),
                ('Size', C.c_ulong), ('ModBase', C.c_ulonglong),
                ('Flags', C.c_ulong), ('Value', C.c_ulonglong),
                ('Address', C.c_ulonglong), ('Register', C.c_ulong),
                ('Scope', C.c_ulong), ('Tag', C.c_ulong),
                ('NameLen', C.c_ulong), ('MaxNameLen', C.c_ulong),
                ('Name', C.c_char*(MAX_SYM_NAME+1))]

CB = C.WINFUNCTYPE(C.c_bool, C.POINTER(SYMBOL_INFO), C.c_ulong, C.c_void_p)

dbg.SymSetOptions(0x00000002 | 0x00000040)
dbg.SymLoadModuleExW.argtypes = [W.HANDLE, W.HANDLE, C.c_wchar_p, C.c_wchar_p,
                                 C.c_ulonglong, C.c_ulong, C.c_void_p, C.c_ulong]
dbg.SymLoadModuleExW.restype = C.c_ulonglong
dbg.SymGetTypeInfo.argtypes = [W.HANDLE, C.c_ulonglong, C.c_ulong, C.c_int, C.c_void_p]

h = W.HANDLE(0x1234)
dbg.SymInitialize(h, None, False)
img = os.path.abspath(sys.argv[1])
base = dbg.SymLoadModuleExW(h, None, img, None, 0x10000000, 0, None, 0)
if not base: raise SystemExit('load failed %d' % C.GetLastError())

TI_SYMTAG, TI_SYMNAME, TI_LENGTH, TI_TYPE, TI_TYPEID = 0, 1, 2, 3, 4
TI_BASETYPE, TI_FINDCHILDREN, TI_OFFSET = 5, 7, 10
TI_COUNT, TI_CHILDRENCOUNT, TI_DATAKIND, TI_UDTKIND = 12, 13, 8, 21

def ti(tid, what, ctype=C.c_ulong):
    v = ctype()
    if not dbg.SymGetTypeInfo(h, base, tid, what, C.byref(v)): return None
    return v.value

def tname(tid):
    p = C.c_void_p()
    if not dbg.SymGetTypeInfo(h, base, tid, TI_SYMNAME, C.byref(p)) or not p.value:
        return None
    s = C.wstring_at(p)
    C.windll.kernel32.LocalFree(p)
    return s

BT = {0:'<none>',1:'void',2:'char',3:'wchar_t',6:'int',7:'unsigned',8:'float',
      9:'bcd',10:'bool',13:'long',14:'unsigned long',25:'currency',29:'bit',
      31:'HRESULT',32:'char16_t',33:'char32_t',34:'char8_t'}

def typestr(tid, depth=0):
    if tid is None or depth > 6: return '?'
    tag = ti(tid, TI_SYMTAG)
    ln  = ti(tid, TI_LENGTH, C.c_ulonglong)
    if tag == 16:  # BaseType
        b = ti(tid, TI_BASETYPE)
        n = BT.get(b, 'bt%s' % b)
        if b in (6,7) and ln: n = {1:'int8',2:'int16',4:'int32',8:'int64'}.get(ln,n)
        if b == 7 and ln: n = 'u' + n
        if b == 8 and ln == 8: n = 'double'
        return n
    if tag == 14:  # Pointer
        return typestr(ti(tid, TI_TYPEID), depth+1) + '*'
    if tag == 15:  # Array
        cnt = ti(tid, TI_COUNT)
        return '%s[%s]' % (typestr(ti(tid, TI_TYPEID), depth+1), cnt)
    if tag in (11, 12, 17):  # UDT / Enum / Typedef
        return tname(tid) or ('tag%d' % tag)
    if tag == 13: return 'func()'
    return tname(tid) or ('tag%s' % tag)

def children(tid):
    n = ti(tid, TI_CHILDRENCOUNT) or 0
    if not n: return []
    class FC(C.Structure):
        _fields_ = [('Count', C.c_ulong), ('Start', C.c_ulong), ('ChildId', C.c_ulong*n)]
    fc = FC(n, 0)
    if not dbg.SymGetTypeInfo(h, base, tid, TI_FINDCHILDREN, C.byref(fc)): return []
    return list(fc.ChildId)

def dump(tid, name):
    ln = ti(tid, TI_LENGTH, C.c_ulonglong)
    print('struct %s {   // %s bytes' % (name, ln))
    for c in children(tid):
        if ti(c, TI_SYMTAG) != 7: continue   # SymTagData only
        off = ti(c, TI_OFFSET, C.c_long)
        cn  = tname(c)
        ct  = typestr(ti(c, TI_TYPEID))
        clen = ti(ti(c, TI_TYPEID), TI_LENGTH, C.c_ulonglong)
        print('  /* %3s */ %-24s %-20s // %s bytes' % (off, ct, cn, clen))
    print('};')

wanted = sys.argv[2:]
found = {}
def cb(psym, size, ctx):
    s = psym.contents
    if s.Tag != 11: return True   # SymTagUDT
    nm = s.Name[:s.NameLen].decode('utf-8','replace') if s.NameLen else s.Name.decode('utf-8','replace')
    found[nm] = s.TypeIndex
    return True
dbg.SymEnumTypes = dbg.SymEnumTypes
dbg.SymEnumTypes.argtypes = [W.HANDLE, C.c_ulonglong, CB, C.c_void_p]
dbg.SymEnumTypes(h, base, CB(cb), None)

if not wanted:
    for nm, tid in sorted(found.items()):
        print('%8s  %s' % (ti(tid, TI_LENGTH, C.c_ulonglong), nm))
    print('# %d UDTs' % len(found), file=sys.stderr)
else:
    for w in wanted:
        hits = [k for k in found if k == w] or [k for k in found if w.lower() in k.lower()]
        if not hits: print('// not found: %s' % w); continue
        for k in hits: dump(found[k], k)
