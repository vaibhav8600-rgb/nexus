#!/usr/bin/env python3
"""Buddy: the rules that keep it from touching anything that already works,
the persona the three brains share, and the PC brain's handling of Gemini.

The face's own timing is tested in C (test_face_anim.c); its pixels by
scripts/render_ui.py. This is everything else: source checks that hold the
feature to "off adds nothing", and the companion's Brain run against a mocked
Gemini - a valid answer, bad JSON, an emotion the dongle does not have, a line
too long, a quota error.

Run: python tests/buddy/test_buddy.py
"""
import importlib.util
import io
import json
import os
import re
import sys
import time
import urllib.error

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
bad = []


def ok(cond, why):
    print('  %s  %s' % ('ok  ' if cond else 'FAIL', why))
    if not cond:
        bad.append(why)


def read(*parts):
    with open(os.path.join(ROOT, *parts), encoding='utf-8') as f:
        return f.read()


def companion():
    spec = importlib.util.spec_from_file_location(
        'nexus_host', os.path.join(ROOT, 'tools', 'nexus-host', 'nexus_host.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def compiled_out():
    print('\nOff adds nothing')
    kc = read('Kconfig')
    ok(re.search(r'config NEXUS_BUDDY\n\tbool[^\n]*\n\tdefault n', kc),
       'Buddy is off unless a config turns it on')
    cm = read('CMakeLists.txt')
    ok(re.search(r'zephyr_library_sources_ifdef\(CONFIG_NEXUS_BUDDY\n'
                 r'(\s+src/buddy/\S+\n)+\)', cm),
       'src/buddy is only compiled with it')

    # Every mention of Buddy outside src/buddy sits inside an #if that
    # selects it - the compile-out test the brief asks for.
    word = re.compile(r'\bnexus_buddy_\w+|\bnexus_screen_buddy_def\b|'
                      r'NEXUS_SOUND_BUDDY_\w+|\bg_soft\b|\bg_blip\b|'
                      r'\bnexus_host_phone_put\b|\bg_from\b|\bhost_phone\b|'
                      r'nexus_screen_paint|nexus_screen_dimmed|'
                      r'\bLIST_ROWS\b|NEXUS_ACTION_BUDDY|'
                      r'k_companion|write_companion')
    unguarded = []
    seen = 0
    for top in ('src', 'include'):
        for dirpath, _, files in os.walk(os.path.join(ROOT, top)):
            for name in files:
                rel = os.path.relpath(os.path.join(dirpath, name), ROOT)
                rel = rel.replace(os.sep, '/')
                if rel.startswith('src/buddy/') or rel.endswith('buddy.h'):
                    continue
                if rel.endswith('action.h') or rel.endswith('dt-bindings/nexus.h'):
                    continue            # an action ID is a number, not code
                stack = []
                for n, line in enumerate(read(rel).splitlines(), 1):
                    d = line.strip()
                    if re.match(r'#\s*if', d):
                        stack.append('CONFIG_NEXUS_BUDDY' in d and '!' not in d)
                    elif re.match(r'#\s*(else|elif)', d) and stack:
                        stack[-1] = False
                    elif re.match(r'#\s*endif', d) and stack:
                        stack.pop()
                    elif word.search(line) and not d.startswith(('/*', '*')):
                        seen += 1
                        # LIST_ROWS is defined from IS_ENABLED, which is
                        # 0 without Buddy: the same 14 rows as before.
                        if not any(stack) and 'LIST_ROWS' not in line:
                            unguarded.append('%s:%d' % (rel, n))
    ok(seen > 20 and not unguarded,
       'all %d uses outside src/buddy are inside #if CONFIG_NEXUS_BUDDY...: %s'
       % (seen, unguarded or 'none outside'))

    menus = read('src', 'ui', 'menus.c')
    ok('#define MAX_ROWS 14' in menus and
       '#define LIST_ROWS (MAX_ROWS + IS_ENABLED(CONFIG_NEXUS_BUDDY))' in menus,
       'Settings keeps 14 rows and its RAM ceiling; Buddy\'s 15th exists '
       'only with Buddy')

    snd = read('src', 'sound', 'sound.c')
    ok('struct note {\n\tuint16_t freq;  /* Hz, 0 = rest */\n\tuint16_t ms;\n};'
       in snd, 'struct note is unchanged, so every existing table is too')
    ok('{E6,90},{REST,25},{E6,90}' in snd, 'and the startup tune with it')

    st = read('src', 'settings.c')
    ok('#define NEXUS_PREFS_VERSION 3' in st,
       'the saved settings keep their version: nobody\'s theme resets')
    bud = read('src', 'buddy', 'buddy.c')
    ok('settings_save_one("nexus/buddy/prefs"' in bud and
       'settings_save_one(NEXUS_PREFS_KEY' not in bud,
       'Buddy saves under its own key')

    scr = bud.split('const struct nexus_screen nexus_screen_buddy_def = {')[1]
    ok('.refresh = NEXUS_REFRESH_NORMAL' in scr.split('};')[0],
       'the face is NORMAL, not FAST: FAST would stop the display blanking')
    ok('nexus_screen_render_now' not in bud,
       'and it paints without counting as input, so it never keeps the '
       'display awake')
    code = re.sub(r'/\*.*?\*/', '', bud + read('src', 'buddy', 'face.c') +
                  read('src', 'buddy', 'face_anim.c'), flags=re.S)
    ok(not re.search(r'\b(k_malloc|malloc|k_heap|float|double|printk|snprintf)\b',
                     code),
       'no heap, no float, no printf in src/buddy')

    status = read('src', 'status', 'status.c')
    slots = int(re.search(r'#define NEXUS_STATUS_MAX_OBS (\d+)', status).group(1))
    users = sum(len(re.findall(r'nexus_status_subscribe\(', read(*f)))
                for f in (('src', 'host', 'host_link.c'),
                          ('src', 'remote', 'remote.c'),
                          ('src', 'ui', 'home.c'),
                          ('src', 'buddy', 'buddy.c')))
    ok(users == 4 and users < slots,
       'four observers with everything built, of %d slots' % slots)


def protocol():
    print('\nThe brain\'s keys belong to the face, never to the HOST screen')
    c = read('src', 'host', 'host_link.c')
    parse = c.split('static void parse_line(const char *line)\n{')[1]
    parse = parse.split('\n}\n')[0]
    ok(parse.index('nexus_buddy_line(g_from, key, val)') < parse.index('switch (key)'),
       'E S V L B are taken before the HOST fields are looked at')
    ok("key != 'T' && key != 'D'" in parse,
       'from the phone, only the clock gets past them')
    tail = parse.split('default:\n\t\treturn;\n\t}')[1]
    ok(tail.index('g_from == NEXUS_BUDDY_PHONE') < tail.index('g_host.link = true'),
       'and the phone never lights LINKED')
    work = c.split('static void parse_work_fn(struct k_work *work)\n{')[1]
    work = work.split('\n}\n')[0]
    ok('drain(&host_phone, true);' in work and 'lines += drain(&host_phone' not in work,
       'the phone is drained whatever the endpoint, and keeps no companion fresh')

    b = read('src', 'buddy', 'buddy.c')
    keys = b.split('bool nexus_buddy_line(')[1].split('\n}\n')[0]
    ok("key != 'E' && key != 'S' && key != 'V' && key != 'L' && key != 'B'" in keys,
       'Buddy claims exactly E S V L B')

    r = read('src', 'remote', 'remote.c')
    svc = r.split('BT_GATT_SERVICE_DEFINE(nexus_remote_svc,')[1].split(');')[0]
    ok(svc.strip().endswith('COMPANION_ATTRS'),
       'the Companion characteristic is the last attribute of the service, so '
       'none before it moves')
    ok('#define STATUS_ATTR (&nexus_remote_svc.attrs[10])' in r,
       'Status is still attribute 10')
    comp = r.split('static ssize_t write_companion(')[1].split('\n}\n')[0]
    ok('gate(' not in comp, 'it works with remote mode off: it cannot type')
    ok('BT_GATT_PERM_WRITE_LESC, NULL, write_companion' in r,
       'and needs the same LESC pairing as everything else in the service')
    ok('IS_ENABLED(CONFIG_NEXUS_BUDDY_PHONE) ? BIT(6) : 0' in r,
       'Status says it is there (features bit 6), so the app can tell')
    ok('#define REMOTE_PROTO_VERSION 1' in read('src', 'remote', 'remote_proto.h'),
       'and no existing byte changed: the protocol version stays 1')


def persona():
    print('\nOne persona for the dongle, the PC and the phone')
    p = json.loads(read('persona', 'persona.json'))
    kc = read('Kconfig')
    name = re.search(r'config NEXUS_BUDDY_NAME\n(?:\t.*\n)*?\tdefault "(\w+)"', kc)
    ok(name and name.group(1) == p['name'],
       'Kconfig\'s default name is persona.json\'s (%s)' % p['name'])
    names = re.search(r'face_emotion_names\[FACE_EMOTIONS\] = \{(.*?)\};',
                      read('src', 'buddy', 'face_anim.c'), re.S).group(1)
    ok(re.findall(r'"(\w+)"', names) == [e.upper() for e in p['emotions']],
       'the firmware\'s emotions are persona.json\'s, in order')
    for reason, pool in p['canned'].items():
        for emotion, line in pool:
            ok(emotion in p['emotions'] and len(line) <= 24 and
               all(32 <= ord(ch) <= 90 for ch in line),
               'canned %s "%s" fits the font and the bubble' % (reason, line))


class FakeGemini:
    def __init__(self, answers):
        self.answers = list(answers)
        self.bodies = []

    def __call__(self, body):
        self.bodies.append(body)
        a = self.answers.pop(0)
        if isinstance(a, Exception):
            raise a
        return a


def reply(obj):
    text = obj if isinstance(obj, str) else json.dumps(obj)
    return json.dumps({'candidates': [{'content': {'parts': [{'text': text}]}}]})


def run(brain, now):
    """Two companion rounds a second apart, the request thread finished in
    between. A fast answer lands in the first round, a slow one in the
    second; either is right, so the pair is what is checked."""
    out = brain.lines(now, 30, 50, ('SO WHAT', 'MILES DAVIS', True))
    for _ in range(500):
        if not brain.inflight:
            break
        time.sleep(0.01)
    return out + brain.lines(now + 1, 30, 50, ('SO WHAT', 'MILES DAVIS', True))


def said(out):
    """The E and S that carry an answer: not B 1, not E THINKING."""
    return [x for x in out if x != 'B 1' and x != 'E THINKING']


def pc_brain():
    print('\nThe PC brain')
    m = companion()
    p = json.loads(read('persona', 'persona.json'))

    ok(m.parse_reply(reply({'emotion': 'HAPPY', 'line': 'Nice one!',
                            'reply': 'x'}), p) == ('happy', 'NICE ONE!'),
       'a valid answer: emotion and line, upper case')
    ok(m.parse_reply('{nope', p) is None, 'bad JSON is refused')
    ok(m.parse_reply(reply('{"emotion": "HAPPY"'), p) is None,
       'and bad JSON inside a good envelope')
    ok(m.parse_reply(reply({'emotion': 'GRUMPY', 'line': 'HI'}), p) is None,
       'an emotion the dongle does not have is refused')
    ok(m.parse_reply(reply({'emotion': 'SAD', 'line': 'X' * 25}), p) is None,
       'a line past 24 characters is refused, not cut')
    ok(m.parse_reply(reply({'emotion': 'SAD', 'line': 'café'}), p) is None,
       'and one the font cannot draw')

    fake = FakeGemini([reply({'emotion': 'LOVE', 'line': 'HI THERE', 'reply': 'r'}),
                       reply({'emotion': 'GRUMPY', 'line': 'NO', 'reply': 'r'}),
                       urllib.error.HTTPError('u', 429, 'quota', {}, io.BytesIO())])
    old = sys.stderr
    sys.stderr = io.StringIO()
    try:
        b = m.Brain(p, 'test-key', call=fake)
        first = run(b, 1000.0)
        ok(first[0] == 'B 1', 'B 1 first, to take the face: %s' % first)
        ok(said(first) == ['E LOVE', 'S HI THERE'],
           'then the answer: %s' % first)
        ok(b.lines(1002.0) == [], 'and nothing more until there is a reason')

        body = json.dumps(fake.bodies[0])
        ok('SO WHAT' in body and 'MILES DAVIS' in body and 'medium' in body,
           'Gemini hears the track and a load bucket')
        ok('test-key' not in body and '"30"' not in body and ' 30%' not in body,
           'never the key in the body, never an exact load figure')

        third = said(run(b, 1000.0 + 600))   # ten minutes on: unprompted
        ok(len(third) == 2 and third[0].startswith('E ') and
           third[0] != 'E GRUMPY' and third[1].startswith('S '),
           'an answer it cannot use becomes a canned line: %s' % third)

        run(b, 1000.0 + 1300)
        ok(b.backoff == 60 and b.next_ask > 2301.0 + 30,
           'a quota error rests it for a minute, and longer each time')
        warned = sys.stderr.getvalue()
    finally:
        sys.stderr = old
    ok('quota' in warned and 'test-key' not in warned,
       'and says so - without the key')

    canned = m.Brain(p, '')
    out = canned.lines(10.0)
    ok(out[0] == 'B 1' and out[1].startswith('E ') and out[2].startswith('S '),
       'no key: canned lines straight away, no request: %s' % out)

    py = read('tools', 'nexus-host', 'nexus_host.py')
    ps = read('tools', 'nexus-host', 'nexus_host.ps1')
    ok("os.environ.get('GEMINI_API_KEY'" in py and '$env:GEMINI_API_KEY' in ps,
       'both take the key from the environment')
    ok("'--gemini-key'" not in py and '$GeminiKey' not in ps,
       'and never as an argument, where the process list shows it')
    ok("b'B 0\\n' if brain" in py and "@('B 0', 'X')" in ps,
       'both hand the face back (B 0) before saying goodbye (X)')
    import tempfile
    tmp = tempfile.mkdtemp()
    path = os.path.join(tmp, 'gemini.json')
    saved = os.environ.pop('GEMINI_API_KEY', None)
    try:
        ok(m.gemini_config(path) == ('', None), 'no file, no variable: no key')
        with open(path, 'w') as f:
            json.dump({'key': ' file-key ', 'model': 'm-1'}, f)
        ok(m.gemini_config(path) == ('file-key', 'm-1'),
           'gemini.json gives the key and, if it says, the model')
        os.environ['GEMINI_API_KEY'] = 'env-key'
        ok(m.gemini_config(path)[0] == 'env-key', 'the environment wins over it')
        del os.environ['GEMINI_API_KEY']
        with open(path, 'w') as f:
            f.write('{"key": ')
        old, sys.stderr = sys.stderr, io.StringIO()
        try:
            broken = m.gemini_config(path)
        finally:
            sys.stderr = old
        ok(broken == ('', None), 'a broken file is ignored, not fatal')
    finally:
        if saved is not None:
            os.environ['GEMINI_API_KEY'] = saved
    ok('LOCALAPPDATA' in py and "if args.buddy or key:" in py,
       'the file lives outside any repo, and a key in it turns Buddy on')
    ok("$KEY_FILE = Join-Path $HOME_DIR 'gemini.json'" in ps and
       'Update-BuddyKey' in ps and 'AddSeconds(30)' in ps,
       'the Windows companion reads the same file, and picks up a key pasted '
       'while it runs')
    un = ps.split('if ($Uninstall) {')[-1].split('\n}\n')[0]
    ok('$_.FullName -ne $KEY_FILE' in un,
       '-Uninstall keeps the key, so a reinstall needs nothing pasted again')
    inst = ps.split('if ($Install) {')[-1].split('\n}\n')[0]
    ok('if (-not (Test-Path $KEY_FILE))' in inst and 'Copy-Item $KEY_FILE' not in inst,
       '-Install writes an empty key file once, and never copies a key anywhere')
    ok('gemini.json' in read('.gitignore'), 'and git ignores one dropped in a repo')

    ok('IPEndPoint([Net.IPAddress]::Loopback, $CHAT_PORT)' in ps and
       'IPAddress]::Any, $CHAT_PORT' not in ps,
       'chat listens on 127.0.0.1 only: nothing off this PC can talk to it')
    ok('IOControl(-1744830452' in ps,
       'a chat window closed early cannot break the inbox (UDP reset off)')
    ok("-notlike '*-Chat*'" in ps.split('function Stop-Companions')[1].split('\n}\n')[0],
       '-Install stops other companions, not an open chat window')
    ok('"%~dp0nexus_host.ps1" -Chat' in read('tools', 'nexus-host', 'chat.cmd'),
       'chat.cmd is the double-click for -Chat')
    ok('$CHAT_MAX' in ps.split('function Receive-BuddyChat')[1].split('\n}\n')[0],
       'a chat message is capped before it reaches Gemini')

    ok(py.count("'gemini-3.1-flash-lite'") == 1 and
       ps.count("'gemini-3.1-flash-lite'") == 1,
       'the model is named once in each')


def main():
    compiled_out()
    protocol()
    persona()
    pc_brain()
    print('\n%s' % ('FAILED (%d)' % len(bad) if bad else 'PASSED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
