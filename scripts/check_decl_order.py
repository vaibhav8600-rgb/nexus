"""File-scope statics used before they are declared."""
import glob
import io
import re

bad = []
files = (sorted(glob.glob('src/**/*.c', recursive=True))
         + sorted(glob.glob('assets/*.c')))
for f in files:
    src = io.open(f, encoding='utf-8').read()
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
    src = re.sub(r'//[^\n]*', '', src)

    decls = {}
    for m in re.finditer(r'^static\s+(?:const\s+)?\w[\w \t\*]*?\b(g_\w+)\s*[\[=;]',
                         src, re.M):
        decls.setdefault(m.group(1), m.start())
    for pat in (r'K_WORK_DELAYABLE_DEFINE\((g_\w+)', r'K_WORK_DEFINE\((g_\w+)'):
        for m in re.finditer(pat, src):
            decls.setdefault(m.group(1), m.start())

    for name, at in decls.items():
        first = src.find(name)
        if first < at:
            bad.append('%s: %s used at offset %d, declared at %d'
                       % (f.replace('\\', '/'), name, first, at))

dupes = []
for f in files:
    src = io.open(f, encoding='utf-8').read()
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
    src = re.sub(r'//[^\n]*', '', src)

    # Two static functions with one name in one file. The compiler calls this
    # "redefinition of" and stops; there is no compiler here, and menus.c
    # shipped a v_host that collided with one added 140 lines above it.
    # Definitions only - a forward declaration ends in ';' and is fine.
    seen = {}
    for m in re.finditer(r'^static\s+(?:inline\s+)?[\w \t\*]+?\b(\w+)'
                         r'\s*\([^;]*?\)\s*\{', src, re.M | re.S):
        name = m.group(1)
        if name in seen:
            dupes.append('%s: %s defined twice' % (f.replace('\\', '/'), name))
        seen[name] = True

print('checked %d files' % len(files))
for b in bad:
    print('  USE BEFORE DECL  ' + b)
if not bad:
    print('  no file-scope static used before its declaration')
for d in dupes:
    print('  REDEFINED  ' + d)
if not dupes:
    print('  no static function defined twice in one file')
bad += dupes
