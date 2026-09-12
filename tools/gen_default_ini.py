# gen_default_ini.py -- embed TombRaiderVR.ini into src/DefaultIni.h.
#
#   python tools\gen_default_ini.py
#
# The DLL writes the ini out when none exists beside it. Embedding the REPO'S
# template rather than dumping the Config struct is deliberate: the comments in
# that file carry most of what was learned tuning this thing, and a generated
# key=value list would throw all of it away.
#
# The only awkward part is MSVC's 16380-byte cap on a single string literal, so
# the text is split across several adjacent raw literals. Adjacent literals are
# concatenated by the compiler with nothing in between, so the seams carry no
# bytes -- but they must fall on line boundaries to stay readable, which is what
# the chunker below does.
import os, sys

HERE  = os.path.dirname(os.path.abspath(__file__))
ROOT  = os.path.dirname(HERE)
SRC   = os.path.join(ROOT, 'TombRaiderVR.ini')
DST   = os.path.join(ROOT, 'src', 'DefaultIni.h')

# Comfortably under MSVC's 16380, so a long comment block added later cannot
# push a chunk over the limit between regenerations.
CHUNK = 15000

raw = open(SRC, 'rb').read()
text = raw.decode('utf-8').replace('\r\n', '\n')

if ')INI"' in text:
    sys.exit('TombRaiderVR.ini contains the raw-literal terminator )INI" -- '
             'change the delimiter in this script before regenerating.')

# Split on line boundaries.
chunks, cur = [], ''
for line in text.splitlines(keepends=True):
    if cur and len(cur) + len(line) > CHUNK:
        chunks.append(cur)
        cur = ''
    cur += line
if cur:
    chunks.append(cur)

out = []
out.append('// DefaultIni.h -- the stock TombRaiderVR.ini, embedded.')
out.append('//')
out.append('// GENERATED FILE. Do not edit by hand: change TombRaiderVR.ini in the repo root')
out.append('// and run  python tools\\gen_default_ini.py')
out.append('//')
out.append('// The DLL writes this out when no ini exists beside it, so a fresh install gets')
out.append('// the documented, working configuration rather than bare struct defaults -- the')
out.append('// comments in the template carry most of what was learned tuning this thing,')
out.append('// and a generated key=value dump would throw all of it away.')
out.append('//')
out.append('// Source: TombRaiderVR.ini, %d bytes, %d lines.' % (len(raw), text.count('\n') + 1))
out.append('#pragma once')
out.append('')
out.append('namespace tr {')
out.append('')
out.append('// Newlines are LF here; the writer expands them to CRLF on the way out.')
out.append('// Split into adjacent literals only because MSVC caps one at 16380 bytes;')
out.append('// the concatenation is a single continuous string and the seams carry no bytes.')
out.append('inline const char* DefaultIniText() {')

body = ''
for i, c in enumerate(chunks):
    opener = '    return R"INI(' if i == 0 else '           R"INI('
    body += opener + c + ')INI"' + ('\n' if i + 1 < len(chunks) else ';\n')
body += '}\n'

out.append('')          # placeholder, replaced below
head = '\n'.join(out[:-1]) + '\n'
tail = '\n} // namespace tr\n'

open(DST, 'w', encoding='utf-8', newline='\n').write(head + body + tail)
print('wrote %s: %d bytes of ini in %d literal(s)' % (DST, len(text), len(chunks)))
