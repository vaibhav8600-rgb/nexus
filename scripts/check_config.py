#!/usr/bin/env python3
"""Check a ZMK config repo against this module's Kconfig.

Zephyr aborts the build on Kconfig warnings, and assigning a symbol the module
does not declare is one:

    config/nexus_dongle.conf:99: warning: attempt to assign the value '150000'
    to the undefined symbol NEXUS_STATUS_LINK_TIMEOUT_MS
    error: Aborting due to Kconfig warnings

So a `.conf` line left behind after a symbol is renamed or removed does not
degrade gracefully - it fails the whole firmware build, in CI, minutes later,
with the real cause buried a hundred lines up the log. Catching it here takes
a second.

Usage:
    python scripts/check_config.py ../zmk-sofle-main/config
    python scripts/check_config.py ../zmk-sofle-main/config --strict

--strict also reports NEXUS_* symbols the module declares that the config
never sets. Those are harmless (the Kconfig default applies) - it is only
useful when you want to see what you are leaving on defaults.
"""

import glob
import os
import re
import sys


def declared_symbols(module_root):
    """Every `config NEXUS_*` this module defines, across all Kconfig files."""
    syms = set()
    for path in glob.glob(os.path.join(module_root, '**', 'Kconfig*'),
                          recursive=True):
        with open(path, encoding='utf-8', errors='replace') as fh:
            for m in re.finditer(r'^\s*config\s+(NEXUS_[A-Z0-9_]+)',
                                 fh.read(), re.M):
                syms.add(m.group(1))
    return syms


def assignments(config_dir):
    """Every uncommented CONFIG_NEXUS_* assignment, with file and line."""
    out = []
    for path in sorted(glob.glob(os.path.join(config_dir, '**', '*.conf'),
                                 recursive=True)):
        with open(path, encoding='utf-8', errors='replace') as fh:
            for lineno, line in enumerate(fh, 1):
                m = re.match(r'\s*CONFIG_(NEXUS_[A-Z0-9_]+)\s*=', line)
                if m:
                    out.append((path, lineno, m.group(1), line.strip()))
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2

    config_dir = argv[1]
    strict = '--strict' in argv[2:]
    module_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    if not os.path.isdir(config_dir):
        print('not a directory: %s' % config_dir)
        return 2

    declared = declared_symbols(module_root)
    if not declared:
        print('found no NEXUS_* symbols under %s - wrong module root?'
              % module_root)
        return 2

    used = assignments(config_dir)
    stale = [(p, n, s, t) for p, n, s, t in used if s not in declared]

    print('module declares %d NEXUS_* symbols' % len(declared))
    print('config sets     %d of them' % len(used))

    if stale:
        print('\n%d assignment(s) Zephyr will refuse to build:' % len(stale))
        for path, lineno, sym, text in stale:
            print('  %s:%d' % (path, lineno))
            print('      %s' % text)
            near = sorted(d for d in declared
                          if d.split('_')[1] == sym.split('_')[1])
            if near:
                print('      module has: %s' % ', '.join(near))
    else:
        print('\nno stale assignments')

    if strict:
        unset = sorted(declared - {s for _, _, s, _ in used})
        print('\n%d symbol(s) left on their Kconfig default:' % len(unset))
        for s in unset:
            print('  CONFIG_%s' % s)

    return 1 if stale else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
