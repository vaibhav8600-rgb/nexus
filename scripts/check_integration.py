"""Final checks: keymap shape for both firmwares, and every source of sound."""
import io, re, sys, os, glob

CFG = '../zmk-sofle-main/config'
fail = []


def ok(cond, msg):
    print(('  ok    ' if cond else '  FAIL  ') + msg)
    if not cond:
        fail.append(msg)


# ---------------------------------------------------------------- keymap ---
print('KEYMAP: both firmwares must get a full 60-position game layer')
km = io.open(os.path.join(CFG, 'sofle.keymap'), encoding='utf-8').read()

# the conditional block must be well formed
# match directives at line start - the phrase also appears in prose above
d_if = [m.start() for m in re.finditer(r'^#ifdef NEXUS_DONGLE', km, re.M)]
d_el = [m.start() for m in re.finditer(r'^#else', km, re.M)]
d_en = [m.start() for m in re.finditer(r'^#endif', km, re.M)]
ok(len(d_if) == 1, 'exactly one #ifdef NEXUS_DONGLE directive')
ok(len(d_el) == 1, 'exactly one #else directive')
ok(len(d_en) == 1, 'exactly one #endif directive')
i, e, n = d_if[0], d_el[0], d_en[0]
ok(i < e < n, '#ifdef / #else / #endif are in order')

nexus_branch, snake_branch = km[i:e], km[e:n]


ALIAS_RE = re.compile(r'^#define\s+([A-Z][A-Z0-9_]*)\s+&', re.M)


def count_bindings(block, aliases):
    """Count DT bindings in the game layer's own `bindings = < ... >`.

    Anchored on the layer node, not just the first `bindings` in the block:
    the NEXUS branch also defines a sensor-rotate behavior whose own
    `bindings = <&nexus_action>, <&nexus_action>;` would otherwise be counted.

    A cell is either `&behavior` plus its parameters, or one of the
    `#define NX_FOO &nexus_action ...` aliases the NEXUS branch uses to keep
    its grid readable. Without expanding those, an alias reads as a parameter
    of the previous binding and the row comes up short.
    """
    at = re.search(r'game_layer\}?\s*\{', block)
    if not at:
        return None
    m = re.search(r'bindings\s*=\s*<([^>]*)>\s*;', block[at.end():], re.S)
    if not m:
        return None
    body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)
    toks = body.split()
    total, k = 0, 0
    while k < len(toks):
        if not (toks[k].startswith('&') or toks[k] in aliases):
            k += 1
            continue
        total += 1
        k += 1
        while k < len(toks) and not (toks[k].startswith('&')
                                     or toks[k] in aliases):
            k += 1
    return total


# the fallback grid inside the keymap node
fallback = km[:i]
fallback = fallback[fallback.rindex('game_layer'):]
aliases = set(ALIAS_RE.findall(km))
print('   (%d keymap aliases expanded: %s...)'
      % (len(aliases), ', '.join(sorted(aliases)[:4])))
for name, block in (('NEXUS branch', nexus_branch),
                    ('snake branch', snake_branch),
                    ('fallback grid', fallback)):
    n_b = count_bindings(block, aliases)
    ok(n_b == 60, '%s has %s bindings (need 60)' % (name, n_b))

ok('&{/keymap/game_layer}' in snake_branch,
   'snake branch overrides the layer by PATH (the node has no label, and the'
   ' name is not unique)')
ok('&snake_dir' in snake_branch, 'snake branch binds the direction keys')
ok('&dongle_action_behavior' in snake_branch, 'snake branch binds the action key')
ok('&anti_idle' in snake_branch, 'snake branch keeps the jiggler toggle')
ok('nexus_action' in nexus_branch, 'NEXUS branch binds nexus_action')
ok('&snake_dir' not in nexus_branch, 'NEXUS branch does not bind snake keys')
ok('sensor-bindings' in snake_branch, 'snake branch keeps its encoders')

# behaviours referenced by the snake branch must be declared unconditionally
for beh in ('snake_dir', 'dongle_action_behavior', 'anti_idle'):
    decl = re.search(r'\b' + beh + r':\s*' + beh + r'\s*\{', km)
    ok(bool(decl), '%s is declared in the keymap' % beh)
    if decl:
        ok(decl.start() < i, '%s is declared outside the #ifdef' % beh)

# ------------------------------------------------------------- sound -------
print('\nSOUND: nothing may play on a timer or on a state NEXUS cannot observe')
plays = []
for f in sorted(glob.glob('src/**/*.c', recursive=True)):
    src = io.open(f, encoding='utf-8').read()
    for m in re.finditer(r'nexus_sound_play\((NEXUS_SOUND_[A-Z_0-9]+)\)', src):
        line = src[:m.start()].count('\n') + 1
        plays.append((f.replace('\\', '/'), line, m.group(1)))

for f, line, snd in plays:
    print('   %-28s :%-4d %s' % (f, line, snd))

srcs = {f: io.open(f, encoding='utf-8').read()
        for f in glob.glob('src/**/*.c', recursive=True)}
allsrc = ''.join(srcs.values())

# the two things that caused spurious beeps
ok('LINK_TIMEOUT' not in allsrc,
   'no silence-derived link timeout anywhere in src/')
ok('ZMK_ACTIVITY_IDLE' not in allsrc,
   'nothing fires on ZMK_ACTIVITY_IDLE (30 s of not typing)')

st = srcs['src\\status\\status.c'] if 'src\\status\\status.c' in srcs \
    else srcs['src/status/status.c']
sweep = st.split('static void stale_work_cb(struct k_work *work)\n{')[1].split('\n}')[0]
ok('nexus_sound_play' not in sweep, 'the periodic sweep plays no sound')
ok('set_link' not in sweep, 'the periodic sweep changes no link state')

# every half chirp must come from the connection callbacks
conn = io.open('src/status/split_conn.c', encoding='utf-8').read()
ok('BT_CONN_CB_DEFINE' in conn, 'half link state comes from bt_conn callbacks')
half_sounds = [s for _, _, s in plays if 'HALF' in s]
ok(len(half_sounds) == 0,
   'no file plays a HALF cue directly - they go through set_link (%s)'
   % (half_sounds or 'none'))
ok('NEXUS_SOUND_HALF_L_CONNECT' in st and 'NEXUS_SOUND_HALF_R_DISCONNECT' in st,
   'set_link owns all four half cues')

# ------------------------------------------------------------- config ------
print('\nCONFIG: no assignment Zephyr will refuse')
kc = ''.join(io.open(f, encoding='utf-8').read()
             for f in glob.glob('**/Kconfig*', recursive=True))
declared = set(re.findall(r'^\s*config\s+(NEXUS_[A-Z0-9_]+)', kc, re.M))
stale = []
for f in glob.glob(os.path.join(CFG, '**', '*.conf'), recursive=True):
    for ln, line in enumerate(io.open(f, encoding='utf-8'), 1):
        m = re.match(r'\s*CONFIG_(NEXUS_[A-Z0-9_]+)\s*=', line)
        if m and m.group(1) not in declared:
            stale.append('%s:%d %s' % (f, ln, m.group(1)))
ok(not stale, 'config repo sets only declared symbols (%s)' % (stale or 'clean'))

# module's own conf files too
mstale = []
for f in glob.glob('boards/**/*.conf', recursive=True):
    for ln, line in enumerate(io.open(f, encoding='utf-8'), 1):
        m = re.match(r'\s*CONFIG_(NEXUS_[A-Z0-9_]+)\s*=', line)
        if m and m.group(1) not in declared:
            mstale.append('%s:%d %s' % (f, ln, m.group(1)))
ok(not mstale, "module's own .conf files are clean (%s)" % (mstale or 'clean'))

print('\n' + ('FAILED: ' + '; '.join(fail) if fail else 'ALL CHECKS PASSED'))
sys.exit(1 if fail else 0)
