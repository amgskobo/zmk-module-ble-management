#!/usr/bin/env python3
"""Complete BLE management handler regressions with real nanopb/Zephyr addresses."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=root/'src/studio/ble_management_handler.c')
parser.add_argument('--labels-source', type=Path, default=root/'src/profile_labels.c')
parser.add_argument('--case', choices=('all','split-info','storage-key','storage-read','labels','profiles',
                                       'forget-split','unlock'), default='all')
args = parser.parse_args()
nanopb = Path(os.environ['NANOPB_SOURCE']) if 'NANOPB_SOURCE' in os.environ else Path(
    subprocess.check_output(['west','list','nanopb','-f','{abspath}'],text=True).strip())
zephyr = Path(os.environ['ZEPHYR_BASE']) if 'ZEPHYR_BASE' in os.environ else Path(
    subprocess.check_output(['west','list','zephyr','-f','{abspath}'],text=True).strip())
with tempfile.TemporaryDirectory(prefix='ble-split-info-') as folder:
    work = Path(folder)
    shutil.copytree(nanopb/'generator',work/'generator')
    subprocess.run(['python3',str(work/'generator/nanopb_generator.py'),'-I',str(root/'proto'),
                    '-D',str(work),str(root/'proto/zmk/ble_management/ble_management.proto')],check=True)
    for rel in ('zephyr/bluetooth/addr.h','zephyr/bluetooth/bluetooth.h','zephyr/bluetooth/conn.h',
                'zephyr/settings/settings.h','zephyr/logging/log.h','zmk/ble.h','zmk/endpoints.h',
                'zmk/studio/core.h','zmk/studio/custom.h'):
        path = work/rel
        path.parent.mkdir(parents=True,exist_ok=True)
        path.write_text('/* Controlled framework API; see handler_test.c. */\n')
    shutil.copyfile(zephyr/'include/zephyr/bluetooth/addr.h',work/'zephyr/bluetooth/addr.h')
    (work/'zephyr/sys').mkdir(parents=True)
    (work/'zephyr/sys/printk.h').write_text('#include <stdbool.h>\n#include <stdio.h>\n#define snprintk snprintf\n')
    (work/'zephyr/sys/util.h').write_text('#include <stddef.h>\n#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))\nsize_t hex2bin(const char *, size_t, unsigned char *, size_t);\n')
    (work/'zephyr/bluetooth/crypto.h').write_text('#include <stddef.h>\nint bt_rand(void *, size_t);\n')
    # Studio RPC, and so this module, runs only standalone or on a split central.
    roles = [('standalone',0,0,5),('central',1,1,5)]
    if args.case in ('all','profiles'): roles.append(('standalone-6-profiles',0,0,6))
    for role, split, central, count in roles:
        builds=[('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'])]
        for label, flags in (builds[::-1] if count>5 else builds):
            binary = work/(role+'-'+label)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                            '-Wno-unused-function','-Wno-unused-variable','-Wno-sign-compare',
                            '-Wno-error=stringop-truncation','-Wno-error=aggressive-loop-optimizations',*flags,
                            '-DCONFIG_ZMK_BLE=1',f'-DCONFIG_ZMK_SPLIT_BLE={split}',
                            f'-DZMK_BLE_PROFILE_COUNT={count}',
                            f'-DCONFIG_ZMK_SPLIT_ROLE_CENTRAL={central}',
                            f'-DHANDLER_SOURCE="{args.source.resolve()}"',
                            f'-DLABELS_SOURCE="{args.labels_source.resolve()}"',f'-DTEST_ROLE="{role}/{label}"',
                            '-I'+str(work),'-I'+str(nanopb),str(root/'tests/split-info/handler_test.c'),
                            str(zephyr/'subsys/bluetooth/common/addr.c'),str(zephyr/'subsys/bluetooth/host/addr.c'),
                            str(work/'zmk/ble_management/ble_management.pb.c'),
                            *(str(nanopb/f'pb_{part}.c') for part in ('common','encode','decode')),
                            '-o',str(binary)],check=True)
            subprocess.run([str(binary),'profiles' if count>5 else args.case],check=True,timeout=20)
