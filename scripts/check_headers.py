#!/usr/bin/env python3
"""Find identifiers used in a .c file whose header is not included.

This exists because of one build failure, and the mechanism is worth stating.
assets/splash_default.c used ARG_UNUSED() without <zephyr/kernel.h>. In C
that is an implicit declaration: the compiler assumes `int ARG_UNUSED()` and
emits a CALL. gcc reports it as a warning, so the object built. ar does not
resolve symbols, so libnexus.a archived cleanly. The build then died at the
final link with "undefined reference to ARG_UNUSED" - hundreds of steps
later, in a log that scrolls past the file at fault.

Every other check here looks at NEXUS symbols, and those all resolved. A
macro borrowed from Zephyr without its header resolves to nothing at all.

Run: python scripts/check_headers.py
"""
import glob
import io
import re
import sys

# identifier -> header that must be included (directly or via another)
NEED = {
    'ARG_UNUSED': 'zephyr/kernel.h',
    'IS_ENABLED': 'zephyr/kernel.h',
    'BIT': 'zephyr/kernel.h',
    'MIN': 'zephyr/kernel.h',
    'MAX': 'zephyr/kernel.h',
    'CLAMP': 'zephyr/kernel.h',
    'ARRAY_SIZE': 'zephyr/kernel.h',
    'BUILD_ASSERT': 'zephyr/kernel.h',
    'k_uptime_get': 'zephyr/kernel.h',
    'k_work_submit': 'zephyr/kernel.h',
    'k_work_schedule': 'zephyr/kernel.h',
    'k_work_reschedule': 'zephyr/kernel.h',
    'k_msgq_put': 'zephyr/kernel.h',
    'LOG_DBG': 'zephyr/logging/log.h',
    'LOG_INF': 'zephyr/logging/log.h',
    'LOG_WRN': 'zephyr/logging/log.h',
    'LOG_ERR': 'zephyr/logging/log.h',
    'memcpy': 'string.h',
    'memcmp': 'string.h',
    'memset': 'string.h',
    'strlen': 'string.h',
    'bt_addr_le_cmp': 'zephyr/bluetooth/conn.h',
    'bt_conn_get_info': 'zephyr/bluetooth/conn.h',
}
# headers that transitively provide the kernel utility macros
KERNEL_PROVIDERS = ('zephyr/kernel.h', 'zephyr/logging/log.h',
                    'zephyr/device.h', 'zephyr/bluetooth/conn.h',
                    'zephyr/settings/settings.h', 'zephyr/sys/util.h',
                    'zephyr/drivers/gpio.h', 'zephyr/drivers/pwm.h',
                    'zephyr/drivers/display.h', 'zephyr/sys/atomic.h')

bad = []
files = sorted(glob.glob('src/**/*.c', recursive=True)) + \
    sorted(glob.glob('assets/*.c'))
for f in files:
    src = io.open(f, encoding='utf-8').read()
    body = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
    body = re.sub(r'//[^\n]*', '', body)
    incs = set(re.findall(r'#include\s+[<"]([^>"]+)[>"]', body))
    has_kernel = any(h in incs for h in KERNEL_PROVIDERS)
    for ident, hdr in NEED.items():
        if not re.search(r'\b' + ident + r'\b', body):
            continue
        if hdr in incs:
            continue
        if hdr == 'zephyr/kernel.h' and has_kernel:
            continue
        if hdr == 'zephyr/logging/log.h' and 'zephyr/logging/log.h' in incs:
            continue
        bad.append('%s: uses %s but does not include %s'
                   % (f.replace('\\', '/'), ident, hdr))

print('checked %d files' % len(files))
if bad:
    for b in bad:
        print('  MISSING  ' + b)
else:
    print('  no missing headers')

sys.exit(1 if bad else 0)
