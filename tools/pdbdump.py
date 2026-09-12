# pdbdump.py -- enumerate symbols (name -> RVA) from a PE + its PDB via dbghelp.
# Usage: python pdbdump.py <image.exe|dll> [name-substring-filter ...]
import ctypes as C, ctypes.wintypes as W, sys, os

dbg = C.WinDLL('dbghelp.dll')
MAX_SYM_NAME = 2000

class SYMBOL_INFO(C.Structure):
    _fields_ = [
        ('SizeOfStruct', C.c_ulong), ('TypeIndex', C.c_ulong),
        ('Reserved', C.c_ulonglong * 2), ('Index', C.c_ulong),
        ('Size', C.c_ulong), ('ModBase', C.c_ulonglong),
        ('Flags', C.c_ulong), ('Value', C.c_ulonglong),
        ('Address', C.c_ulonglong), ('Register', C.c_ulong),
        ('Scope', C.c_ulong), ('Tag', C.c_ulong),
        ('NameLen', C.c_ulong), ('MaxNameLen', C.c_ulong),
        ('Name', C.c_char * (MAX_SYM_NAME + 1)),
    ]

CB = C.WINFUNCTYPE(C.c_bool, C.POINTER(SYMBOL_INFO), C.c_ulong, C.c_void_p)

SYMOPT_UNDNAME       = 0x00000002
SYMOPT_DEBUG         = 0x80000000
SYMOPT_LOAD_ANYTHING = 0x00000040
SYMOPT_EXACT_SYMBOLS = 0x00000400

dbg.SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_ANYTHING)
dbg.SymInitialize.argtypes = [W.HANDLE, C.c_char_p, C.c_bool]
dbg.SymLoadModuleExW.argtypes = [W.HANDLE, W.HANDLE, C.c_wchar_p, C.c_wchar_p,
                                 C.c_ulonglong, C.c_ulong, C.c_void_p, C.c_ulong]
dbg.SymLoadModuleExW.restype = C.c_ulonglong
dbg.SymEnumSymbolsW = dbg.SymEnumSymbols
dbg.SymEnumSymbols.argtypes = [W.HANDLE, C.c_ulonglong, C.c_char_p, CB, C.c_void_p]

h = W.HANDLE(0x1234)
if not dbg.SymInitialize(h, None, False):
    raise SystemExit('SymInitialize failed %d' % C.get_last_error())

img = os.path.abspath(sys.argv[1])
BASE = 0x10000000
base = dbg.SymLoadModuleExW(h, None, img, None, BASE, 0, None, 0)
if not base:
    raise SystemExit('SymLoadModuleEx failed %d' % C.GetLastError())

filters = [f.lower() for f in sys.argv[2:]]
out = []
def cb(psym, size, ctx):
    s = psym.contents
    name = s.Name[:s.NameLen].decode('utf-8', 'replace') if s.NameLen else s.Name.decode('utf-8','replace')
    if filters and not any(f in name.lower() for f in filters):
        return True
    out.append((s.Address - base, s.Size, s.Tag, name))
    return True

if not dbg.SymEnumSymbols(h, base, b'*', CB(cb), None):
    raise SystemExit('SymEnumSymbols failed %d' % C.GetLastError())

# Tag 5 = SymTagFunction, 7 = SymTagData, 10 = SymTagPublicSymbol
TAGS = {5: 'func', 7: 'data', 10: 'public'}
out = [o for o in out if 0 <= o[0] < 0x40000000]
for rva, size, tag, name in sorted(out):
    print('0x%08X  %-7s %6d  %s' % (rva, TAGS.get(tag, str(tag)), size, name))
print('# %d symbols' % len(out), file=sys.stderr)
