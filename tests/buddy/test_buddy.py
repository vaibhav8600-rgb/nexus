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


def face_colours():
    face = read('src', 'buddy', 'face.c')
    ok('t->value : col' in face and 'theme_slot(t, c)' in face,
       'a face colour that would vanish into the ground (AMOLED has no glow) '
       'is drawn in the text colour')
    ok('bud_visible' in read('scripts', 'render_ui.py'),
       'and the docs renderer does the same')


def fn_body(src, signature):
    """One function's body. Not its forward declaration: that ends in ';'."""
    m = re.search(re.escape(signature) + r'[^;{]*\{(.*?)\n\}\n', src, re.S)
    return m.group(1) if m else ''


def who_has_the_face():
    print('\nA brain has the face only while it speaks')
    bud = re.sub(r'/\*.*?\*/', '', read('src', 'buddy', 'buddy.c'), flags=re.S)

    ok('g_brain_mood != FACE_NEUTRAL' in
       fn_body(bud, 'static bool brain_speaking(void)'),
       'one definition: it holds the lease and is showing or saying something')
    ok('if (brain_speaking())' in
       fn_body(bud, 'static enum face_emotion evaluate('),
       'a silent brain leaves Buddy its own moods (a companion renews the '
       'lease every 10 s; that used to switch them off for good)')
    ok('if (!brain_speaking())' in fn_body(bud, 'static bool buddy_action('),
       'and its pokes')
    ok('if (brain_speaking())' in fn_body(bud, 'static void release('),
       'a brain that had gone quiet leaves without the sad face - a phone\'s '
       'lease runs out a minute after every chat')
    ok('g_brain_spoke' not in bud, 'with no second flag to disagree with it')

    enter = fn_body(bud, 'static void buddy_enter(void)')
    ok('now - g_line_at > LINE_FRESH' in enter and 'release(now)' in enter,
       'opening the face drops a line said while it was away, and an '
       'expired lease, instead of replaying them')
    ok('k_uptime_get() >= g_lease_until' in
       fn_body(bud, 'const char *nexus_buddy_brain_name(void)'),
       'BRAIN: does not name a brain whose lease has run out')

    ok('return !brains && typing();' in
       fn_body(bud, 'static bool hush(bool brains)') and
       'hush(g_cue_brain)' in bud and 'hush(g_line_brain)' in bud,
       'typing hushes Buddy\'s own sounds by whose sound it is, not by who '
       'holds the lease')
    ok('!brains && !g_was_brain' in fn_body(bud, 'static void anim_fn('),
       'settling back from a brain\'s face to Buddy\'s own makes no sound')

    snd = re.sub(r'/\*.*?\*/', '', read('src', 'sound', 'sound.c'), flags=re.S)
    ok('g_seq != NULL && g_seq != g_blip' in
       fn_body(snd, 'void nexus_sound_blip('),
       'a letter\'s blip plays into silence or over another blip only: it '
       'used to cut every face\'s cue to a tick in CHATTY')


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

    def takes(line):
        k, v = line[0], line[2:]
        if line[1:2] != ' ' or k not in 'ESL':
            return False
        if k == 'E':
            return v.lower() in p['emotions']
        if k == 'S':
            return 0 < len(v) <= 24 and all(32 <= ord(ch) <= 90 for ch in v)
        m = re.fullmatch(r'(-?\d+),(-?\d+)', v)
        return bool(m) and all(abs(int(n)) <= 20 for n in m.groups())

    c = p['commands']
    for f in c['faces']:
        ok(takes('E ' + f['emotion'].upper()) and takes('S ' + f['line']),
           'face command "%s" is lines the dongle takes' % f['emotion'])
    for a in c['actions']:
        ok(a['steps'] and all(takes(line) for st in a['steps'] for line in st),
           'trick "%s" is lines the dongle takes' % a['words'][0])
    app = os.path.join(ROOT, '..', 'nexus-remote', 'src', 'buddy', 'persona.json')
    if os.path.exists(app):
        ok(json.load(open(app, encoding='utf-8')) == p,
           'NEXUS Remote\'s copy of persona.json is this one')


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
       ps.count("[string]$GeminiModel = 'gemini-3.1-flash-lite'") == 1,
       'the model is named once in each')
    ask = ps.split('function Start-BuddyAsk')[1].split('\n}\n')[0]
    ok('@($GeminiModel) + $LITE_MODELS' in ask and 'foreach ($url in $urls)' in ask and
       '$last.code -in 400, 401, 403' in ask,
       'a remark or chat goes down the Flash Lite chain when a model is busy, out of '
       'quota or gone - but not on a bad key')


def voice():
    print('\nvoice.cmd: Gemini Live, and the relay that moves the face')
    ps = read('tools', 'nexus-host', 'nexus_host.ps1')
    m = re.search(r"\$VOICE_CS = @'\r?\n(.*?)\r?\n'@", ps, re.S)
    ok(m, 'the voice C# is one here-string both modes compile')
    cs = m.group(1) if m else ''

    ok('new UdpClient(new IPEndPoint(IPAddress.Loopback, port))' in cs and
       'new UdpClient(new IPEndPoint(IPAddress.Loopback, 0))' in cs and
       'IPAddress.Any' not in cs,
       'voice UDP binds 127.0.0.1 only: nothing off this PC can move the face')
    ok("-notlike '*-Voice*'" in ps.split('function Stop-Companions')[1].split('\n}\n')[0],
       '-Install stops other companions, not an open voice window')
    ok('"%~dp0nexus_host.ps1" -Voice' in read('tools', 'nexus-host', 'voice.cmd'),
       'voice.cmd is the double-click for -Voice')

    url = re.search(r"\$VOICE_URL = '([^']*)'", ps)
    ok(url and url.group(1).startswith('wss://generativelanguage.googleapis.com/') and
       'key' not in url.group(1).lower(),
       'the Live URL carries no key')
    ok('SetRequestHeader("x-goog-api-key", key)' in cs and '?key=' not in ps,
       'the key goes in the x-goog-api-key header, never in the wss URL')

    rx = re.search(r'Regex\(@"([^"]*)"\)', cs)
    ok(rx and rx.group(1) == r'^(E [A-Z]{1,12}|V [0-4])$',
       'the relay has one allow-list: %s' % (rx and rx.group(1)))
    if rx:
        allowed = re.compile(rx.group(1))
        let = ['E HAPPY', 'E LISTENING', 'V 0', 'V 4']
        stop = ['V 5', 'V 10', 'S HELLO', 'T 48720', 'B 0', 'X', 'e happy',
                'E HAPPY\nT 1', 'E THISISTOOLONGX', 'L -12,8']
        ok(all(allowed.match(s) for s in let) and
           not any(allowed.fullmatch(s) for s in stop),
           'and it lets only E and V lines through to the dongle')
    ok('[Threading.Monitor]::Enter($sp)' in ps and 'lock (p)' in cs,
       "the relay and the main loop share each port's lock")
    ok("$script:relay.Close()" in ps.split('} finally {')[-1],
       'the relay closes when the companion does')
    ok('$script:relay.BleOn = [bool]$ble' in ps and 'Send-VoiceBle $ble' in ps and
       'if (BleOn && Ble.Count < 64) Ble.Enqueue(line);' in cs,
       'over Bluetooth the relay queues its lines, and the main loop sends them '
       'between rounds')
    ok("$LIVE_VOICE = 'Achernar'" in ps and '$($v.spoken)' in ps,
       'Buddy speaks in a soft voice, in the persona\'s spoken style')

    def between(start, end='\n}\n'):
        """The source from the first `start` to the next `end`."""
        return ps.split(start, 1)[1].split(end, 1)[0]

    # PowerShell names ignore case: "$chat = ..." in the window once tried to
    # turn the chat client into the -Chat switch, and voice never started.
    params = re.findall(r'^\s*\[[^\]]+\]\s*\$(\w+)', between('param(', '\n)\n'), re.M)
    window = between('\nif ($Voice) {')
    clash = sorted({n for n in params
                    if re.search(r'(?<![\w:])\$%s\s*=[^=]' % n, window, re.I)})
    ok(params and not clash,
       'the voice window assigns to no variable named like a parameter: %s' % clash)
    ok('BuddyChat($CHAT_PORT)' in window and '.SendText(' in window,
       'typed messages go into the spoken conversation, or to the companion chat')
    ok('-like \'*nexus_host.ps1*-Voice*\'' in window and 'AppActivate' in window,
       'a second voice.cmd brings the open window forward instead of a second one')

    # The window's creature makes every face the dongle makes.
    poses = set(re.findall(r'^\s{4}([A-Z]+)\s+= @\{ eyes', between('$VOICE_POSES = @{'), re.M))
    want = {e.upper() for e in json.loads(read('persona', 'persona.json'))['emotions']}
    ok(poses == want,
       "a pose for each of the dongle's faces, and no other: missing %s, extra %s"
       % (sorted(want - poses), sorted(poses - want)))
    parts = set(re.findall(r"'(\w+)'", between('$VOICE_PARTS = @(', ')')))
    named = set(re.findall(r'x:Name="(\w+)"', between("$VOICE_XAML = @'", "'@")))
    used = (set(re.findall(r"(?:eyes|mouth) = '(\w+)'", ps))
            | set(re.findall(r"'(Prop\w+|Tear)'", ps)))
    ok(parts <= named and used <= parts,
       'every part a pose shows is drawn: %s' % sorted((parts - named) | (used - parts)))
    ok("Send-BuddyMirror $lines" in ps and 'IPAddress]::Loopback, $MIRROR_PORT' in ps and
       'class BuddyMirror' in cs and '^([EB] [A-Z0-9]{1,12}|V [0-4]|L' in cs,
       "the companion copies Buddy's lines to the window, on 127.0.0.1, "
       'and the window takes only those')

    # A typed question about this PC is answered by the PC itself; its
    # numbers go to Gemini only when spoken, because Gemini says the answer.
    ok("name = 'get_pc_stats'" in window and 'fn == "get_pc_stats"' in cs,
       'a spoken question about this PC is answered through get_pc_stats')
    ask = between('function Start-BuddyAsk')
    ok('Machine' not in ask, "a typed chat never sends the PC's numbers to Gemini")
    ok('$cmd = Get-StatsReply $msg.text' in between('function Get-BuddyLines'),
       'the companion answers a PC question itself, like a face or a trick')
    ok('$stats = Get-StatsReply $text' in window and 'Show-VoiceStats' in window,
       'the window sends a PC question to the companion, not to Gemini Live, '
       'and answers it itself without one')
    m = re.search(r"\$STATS_ASK = ((?:'[^']*(?:''[^']*)*'\s*\+?\s*)+)", ps)
    words = (''.join(re.findall(r"'((?:[^']|'')*)'", m.group(1))).replace("''", "'")
             if m else 'x^')
    asked = ["how's my laptop doing?", 'how is my pc', 'what is my cpu at', 'battery?',
             'how much RAM is free', "what's using my memory?", 'disk space', 'show me my stats',
             'memory usage?', 'how much storage do I have', "what's my laptop's battery",
             'pc specs']
    chat = ['hi niku', 'sing me a song', 'how are you today', 'show me a happy face',
            'high five', 'what time is it', 'tell me about computers in space',
            'I have a bad memory today', 'do you remember our first memory?',
            'my social battery is low', 'what are your stats in that game',
            'how is the education system', 'I am charging my phone']
    missed = [t for t in asked if not re.search(words, t, re.I)]
    taken = [t for t in chat if re.search(words, t, re.I)]
    ok(not missed and not taken, 'what counts as asking about this PC: %s' % (missed + taken))

    # It opens at sign-in, quietly, and can be stopped from doing so.
    inst = ps.split('if ($Install) {')[-1].split('\n}\n')[0]
    un = ps.split('if ($Uninstall) {')[-1].split('\n}\n')[0]
    ok('Set-WindowAutostart $true' in inst and
       '-Voice -Idle' in between('function Set-WindowAutostart'),
       '-Install opens the window at sign-in, without the mic (-Idle)')
    ok('Set-WindowAutostart $false' in un and "-like '*nexus_host.ps1*-Voice*'" in un,
       '-Uninstall undoes that and closes the window')
    ok("if (-not $Idle) { Start-VoiceTalk }" in window,
       'an -Idle window waits for a tap before listening')

    # Nobody at the mic, no conversation: it never streams an empty room.
    ok('SessionSwitchReason.SessionLock' in cs and 'PowerModes.Suspend' in cs and
       'SystemEvents.SessionSwitch -= OnSessionSwitch' in cs,
       'locking or sleeping the PC ends the conversation, and Stop() unsubscribes')
    ok('IdleMs = 120000' in cs and '> IdleMs)' in cs,
       'two minutes with nobody talking ends it')
    ok('> MicStallMs) { End(gone)' in cs and
       'waveInAddBuffer(hwi, h, HDR) != 0) { End(gone)' in cs,
       'a mic that is pulled out ends it instead of hanging')
    # Down the chain instead of stopping: each Live model, then Flash Lite.
    chain = re.search(r"\$LIVE_CHAIN = @\(([^)]*)\)", ps)
    ok(chain and re.findall(r"'([^']+)'", chain.group(1)) ==
       ['gemini-3.8-live', 'gemini-3.1-flash-live-preview',
        'gemini-2.5-flash-native-audio-preview-12-2025'],
       'the Live chain is 3.8, then 3.1 Flash, then 2.5 native audio')
    ok("$script:tiers = @(@(@($model) + $LIVE_CHAIN | Select-Object -Unique) + 'lite')"
       in window, 'gemini.json\'s model first, Flash Lite last')
    ok(r'\bquota\b|rate.?limit|exhaust|\b429\b' in cs and "'quota|rate" not in ps,
       'quota is told apart, and never matched on "rate" inside "generate"')
    stop = between('function Stop-VoiceTalk')
    ok("'quota' { $script:rest[$m] = $now.AddMinutes(10) }" in stop and
       'Start-VoiceTalk -Carry' in stop and 'if ($Closing -or -not $kind) { return }' in stop and
       '$script:restarts.Count -ge 6' in stop,
       'a failed model rests and the next carries on - never when ended on purpose '
       'or closing, and at most six restarts a minute')
    ok('Stop-VoiceTalk -Closing' in window, 'closing the window never starts a new conversation')
    setup = between('function Get-VoiceSetup')
    ok('googleSearch' not in setup and 'set_face' not in setup,
       'no Google Search (the free tier refuses 3.8 and 3.1 Live with it) and no '
       'set_face round trip each reply')
    ok('sessionResumption = if ($handle)' in setup and
       'contextWindowCompression = @{ slidingWindow' in setup and
       'The conversation so far' in setup,
       'a reconnect resumes (3.8) or is told the conversation so far; old context is '
       'trimmed so long chats cost no more a turn')
    ok('if (m.ContainsKey("goAway") && goAwayAt == 0) goAwayAt = Now();' in cs and
       'Finish("goaway", null)' in cs,
       "Google's routine disconnect ends in a pause, and the window reconnects")
    ok('PrebufferMs = 120' in cs and 'Write(TakeHeld())' in cs,
       'a reply is held 120 ms before it plays, so a late chunk leaves no gap')
    ok('FaceFor(said.ToString())' in cs and 'FaceEveryMs' in cs,
       'the face follows the words, at most one change (and chirp) every 2.5 s')
    # Faces and tricks asked for out loud play as when typed: set_face used to
    # carry them, and without it "show me an angry face" did nothing.
    log = between('function Read-VoiceLog')
    send = between('function Send-VoiceMessage')
    act = between('function Invoke-VoiceCommand', '\n    }\n')
    ok('Invoke-VoiceCommand $m.Substring(5)' in log and 'Invoke-VoiceCommand $text' in send and
       '$script:persona = $p' in window,
       'a face or trick asked for, spoken or typed mid-conversation, goes to the matcher')
    ok('Get-BuddyCommand $text' in act and '$script:bv.HoldFaces(' in act and
       '$script:cmdClient.Ask($text)' in act and 'chatClient' not in act and
       '$script:cmdClient.Replies.TryDequeue([ref]$r)) {}' in window,
       'the companion plays it on the dongle; its answer comes back on a client of its '
       'own and is never shown, so it cannot take a typed chat\'s place')
    ok('Invoke-VoiceCommand $script:pending' in window,
       'a message typed while connecting is matched too, once it is sent')
    ok('if (r.Contains("already connected")) return "drop";' in cs and
       'ws.CloseOutputAsync(WebSocketCloseStatus.NormalClosure' in cs and
       "$script:resumed -and $kind -in 'refused', 'drop'" in window,
       'a resume that comes back too soon is retried with the conversation told, '
       'never rests 3.8 for an hour; and Stop says goodbye so Google lets go sooner')
    prompt = ps.split('function Get-BuddyPrompt')[1].split('\n}\n')[0]
    ok('your face really does it on the dongle by itself' in prompt and
       "never say you can't" in prompt,
       'Gemini is told the face really happens, so it plays along instead of '
       '"I don\'t know how to be angry"')
    ok('if (Held()) return;' in cs and 'if (!Held()) Face("NEUTRAL");' in cs,
       'a held trick is not overwritten by a face from the words')
    ok('if (heard.Length > 0) { Say("you: " + heard.ToString().Trim()); heard.Clear(); }' in cs,
       'what you said is known when Buddy starts answering, not after')
    ok('System.Speech' in ps and 'Speak(reply)' in cs and 'Hear(pcm)' in cs and
       'foreach (string model in liteModels)' in cs,
       'Flash Lite: sentences cut here, heard and answered down its own chain, said in '
       'the Windows voice')

    # Ctrl+Alt+Shift+N toggles Talk from anywhere; never plain Ctrl+Alt+N.
    ok('$TALK_MODS = 7' in ps and '$TALK_VK = 0x4E' in ps and "'Ctrl+Alt+Shift+N'" in ps,
       'the Talk hotkey is Ctrl+Alt+Shift+N, not VS Code\'s Ctrl+Alt+N')
    ok('MOD_NOREPEAT' in cs and 'UnregisterHotKey' in cs,
       'a held key is one press, and the hotkey is given back')
    ok('$script:hotkey.Take() -gt 0' in window and '$script:hotkey.Close()' in window and
       'is taken by another app' in window,
       'the window toggles Talk on the hotkey, frees it on close, says when it is taken')
    ok('LC(LA(LS(N)))' in read('docs', 'buddy.md'), 'the docs give the ZMK binding for it')

    # With the dongle away, a chat is still answered - the window's face
    # shows it - but Buddy makes no remarks to an empty room.
    loop = ps.split("Write-Output 'NEXUS companion running.")[1]
    waiting = loop.split('if ($open.Count -eq 0 -and -not $ble) {', 1)[1].split('continue', 1)[0]
    ok("Get-BuddyLines $null $null @('', '', 0) -ChatOnly" in waiting and
       'Send-BuddyMirror' in waiting,
       'with no dongle the companion still answers chat, into the window')
    ok('if (-not $ChatOnly -and -not $script:bud.job' in ps,
       'and -ChatOnly makes no unprompted remarks')
    ok('$code -eq 503' in ask, 'a busy Gemini (503) is retried before Buddy gives up')

    # Its pictures in the docs are its own renders.
    docs = read('docs', 'buddy.md')
    for name in ('hero', 'faces', 'window-chat', 'window-talking', 'window-listening'):
        ok(os.path.isfile(os.path.join(ROOT, 'docs', 'images', 'voice', name + '.png')) and
           'images/voice/%s.png' % name in docs,
           'docs/images/voice/%s.png exists and buddy.md shows it' % name)

    # The mouth's RMS -> level mapping, run for real where PowerShell is.
    if os.name != 'nt' or not cs:
        print('  skip  BuddyVoice.Level (needs Windows PowerShell)')
        return
    import subprocess
    import tempfile
    tmp = tempfile.mkdtemp()
    src = os.path.join(tmp, 'voice.cs')
    with io.open(src, 'w', encoding='utf-8') as f:
        f.write(cs)
    script = os.path.join(tmp, 'level.ps1')
    with io.open(script, 'w', encoding='utf-8') as f:
        f.write('\n'.join([
            "Add-Type -TypeDefinition (Get-Content -Raw $args[0]) "
            "-ReferencedAssemblies System.Web.Extensions, System.Speech -ErrorAction Stop",
            # 50 ms at 24 kHz: silence, a quiet sine, a loud one.
            "function Get-Sine($amp) {",
            "  $b = New-Object byte[] 2400",
            "  for ($i = 0; $i -lt 1200; $i++) {",
            "    $s = [int16]($amp * [math]::Sin(2 * [math]::PI * 220 * $i / 24000))",
            "    $b[2 * $i] = $s -band 0xFF; $b[2 * $i + 1] = ($s -shr 8) -band 0xFF",
            "  }",
            "  , $b",
            "}",
            "$out = foreach ($a in 0, 1000, 12000) {",
            "  $b = Get-Sine $a",
            "  [NexusVoice.BuddyVoice]::Level($b, 0, $b.Length)",
            "}",
            "$out -join ' '",
            # The face from the words: the last feeling shown, none for plain facts.
            "$v = [NexusVoice.BuddyVoice]",
            "(@('Haha, so fun!', 'Oh no, I am sorry.', 'Sorry... haha, kidding!',",
            "   'CPU is at 12%.') |",
            "  ForEach-Object { \"$($v::FaceFor($_))\" }) -join ','",
            # What a close means for the next step of the chain.
            "(@('You exceeded your current quota', 'models/x is not found for API version',",
            "   'The service is currently unavailable', 'Failed to generate content',",
            "   'PolicyViolation: session is already connected to an existing client') |",
            "  ForEach-Object { $v::Classify($_) }) -join ','",
        ]))
    err = ''
    try:
        r = subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                            '-File', script, src],
                           capture_output=True, text=True, timeout=120)
        out, err = r.stdout.strip().splitlines(), r.stderr.strip()[:300]
    except (OSError, subprocess.TimeoutExpired) as e:
        out = ['error: %s' % e]
    out += [''] * 3
    ok(out[0] == '0 1 4',
       'the mouth: silence closed, a quiet sine at 1, a loud one wide open (%s %s)'
       % (out[0], err))
    ok(out[1] == 'HAPPY,SAD,HAPPY,',
       'the face follows the last feeling in the words, none for plain facts (%s)' % out[1])
    ok(out[2] == 'quota,refused,busy,drop,drop',
       'a close is told apart as quota, refused, busy or a drop (%s)' % out[2])


def main():
    compiled_out()
    face_colours()
    who_has_the_face()
    protocol()
    persona()
    pc_brain()
    voice()
    print('\n%s' % ('FAILED (%d)' % len(bad) if bad else 'PASSED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
