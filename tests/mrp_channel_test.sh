#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
python3 - "${1:-build}" <<'PY'
import os, pathlib, shlex, subprocess, sys, tempfile
root = pathlib.Path.cwd()
build = (root / sys.argv[1]).resolve()
rule = 'mrp-test-vars:\n\t@echo $(shairport_sync_OBJECTS)\n\t@echo $(LIBS) $(shairport_sync_LDADD)\n\t@echo $(CFLAGS)'
output = subprocess.check_output(['make','--no-print-directory','-s','--eval='+rule,'mrp-test-vars'],cwd=build,text=True).splitlines()
objects, libs, flags = map(shlex.split, output[-3:])
with tempfile.TemporaryDirectory() as directory:
    renamed = pathlib.Path(directory)/'shairport.o'
    subprocess.run(['objcopy','--redefine-sym','main=shairport_application_main',str(build/'shairport.o'),str(renamed)],check=True)
    objects = [str(renamed) if o == 'shairport.o' else str(build/o) for o in objects]
    libraries = [str(build/o) if o.endswith('.a') else o for o in libs]
    binary = pathlib.Path(directory)/'mrp_channel_test'
    subprocess.run([os.environ.get('CC','cc'),'-DHAVE_CONFIG_H','-I'+str(build),'-I'+str(root),
        '--include=utilities/debug.h',*shlex.split(os.environ.get('CPPFLAGS','')),*flags,
        str(root/'tests/mrp_channel_test.c'),*objects,*libraries,
        *shlex.split(os.environ.get('LDFLAGS','')),'-pthread','-lstdc++','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=20)
PY
