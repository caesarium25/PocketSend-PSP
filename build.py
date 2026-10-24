"""Build with an existing PSPDEV directory, without requiring make.
Usage: python build.py /path/to/pspdev
"""
import os
import pathlib
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parent
sdk = pathlib.Path(sys.argv[1]).resolve()
suffix = '.exe' if os.name == 'nt' else ''
env = dict(os.environ)
env['PATH'] = str(sdk / 'bin') + os.pathsep + env['PATH']
env['PSPDEV'] = str(sdk)
build = root / 'build'
build.mkdir(exist_ok=True)

def run(tool, *args):
    subprocess.run([str(sdk/'bin'/(tool+suffix)), *[str(a).replace(chr(92), chr(47)) for a in args]], cwd=sdk, env=env, check=True)

objects = []
for name in ['main', 'ui', 'network', 'core', 'cJSON', 'sha256', 'gfx', 'assets']:
    obj = build / (name+'.o')
    run('psp-gcc', '-O2', '-G0', '-Wall', '-Wextra', '-std=gnu99',
        '-D_PSP_FW_VERSION=661', '-DCJSON_NESTING_LIMIT=32', '-Ipsp/sdk/include', '-Ipsp/include',
        '-c', root/'src'/(name+'.c'), '-o', obj)
    objects.append(obj)
run('psp-gcc', '-G0', '-Lpsp/sdk/lib', '-Lpsp/lib', '-specs=psp/sdk/lib/prxspecs',
    '-Wl,-q,-Tpsp/sdk/lib/linkfile.prx', '-Wl,-zmax-page-size=128',
    *objects, 'psp/sdk/lib/prxexports.o',
    '-lpsppower', '-lm', '-lpspdebug', '-lpspdisplay', '-lpspge', '-lpspctrl',
    '-lpspnet', '-lpspnet_apctl', '-o', build/'pocketsend.elf')
run('psp-fixup-imports', build/'pocketsend.elf')
run('psp-prxgen', build/'pocketsend.elf', build/'pocketsend.prx')
run('mksfoex', '-d', 'MEMSIZE=1', 'PocketSend PSP', build/'PARAM.SFO')
run('pack-pbp', root/'EBOOT.PBP', build/'PARAM.SFO', root/'assets/ICON0.PNG', 'NULL', 'NULL', root/'assets/PIC1.PNG',
    'NULL', build/'pocketsend.prx', 'NULL')
print('Built', root/'EBOOT.PBP')
