"""Exercise the actual VPS updater with a fake Docker daemon and release download."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BASH = r'C:\Program Files\Git\bin\bash.exe'
DOCKER = r'''#!/bin/bash
set -eu
echo "$*" >> "$FIXTURE/calls"
case "$1" in
 inspect) if [ "$#" -gt 2 ]; then echo true; fi ;;
 cp)
   if [[ "$2" == *:/usr/bin/hysteria ]]; then cp "$FIXTURE/binary" "$3"
   elif [[ "$3" == *:/usr/bin/hysteria ]]; then cp "$2" "$FIXTURE/binary"
   else cp "$2" "$FIXTURE/staged"; fi ;;
 exec)
   case "$3" in
     uname) echo "${ARCH:-x86_64}" ;;
     sh) echo 'unchanged configuration checksum' ;;
     mv) mv "$FIXTURE/staged" "$FIXTURE/binary" ;;
     pidof) if [ "$SCENARIO" = unhealthy ]; then exit 1; fi; echo 123 ;;
     /usr/bin/hysteria.selfvps-new) echo 'Version: v2.12.2' ;;
     /usr/bin/hysteria) echo 'Version: v2.12.2' ;;
     *) exit 91 ;;
   esac ;;
 restart|start|stop) echo amnezia-hysteria2 ;;
 *) exit 92 ;;
esac
'''
CURL = '''#!/bin/bash
while [ "$1" != -o ]; do shift; done
printf new > "$2"
'''
SHA = '''#!/bin/bash
if [ "${1:-}" = -c ]; then
  cat >/dev/null
  [ "$SCENARIO" != corrupt ]
else
  cat >/dev/null
  echo unchanged
fi
'''
for scenario in ['success', 'corrupt', 'unhealthy', 'unsupported']:
    with tempfile.TemporaryDirectory(prefix='selfvps-update-test-') as folder:
        work = Path(folder)
        for name, body in [('docker', DOCKER), ('curl', CURL), ('sha256sum', SHA), ('sleep', '#!/bin/bash\nexit 0\n')]:
            (work / name).write_text(body, encoding='utf-8', newline='\n')
        (work / 'binary').write_text('old')
        env = dict(os.environ, FIXTURE=work.as_posix(), SCENARIO=scenario, ARCH='mips' if scenario=='unsupported' else 'x86_64')
        # Let bash translate the Windows path before prepending it to PATH.
        result = subprocess.run([BASH, '-c', 'export PATH="$(cygpath -u "$FIXTURE"):$PATH"; bash "$1"', 'test',
                                 (ROOT / 'client/server_scripts/update_hysteria2.sh').as_posix()], env=env, capture_output=True, text=True)
        assert (result.returncode == 0) == (scenario == 'success'), (scenario, result.stdout, result.stderr)
        assert (work/'binary').read_text() == ('new' if scenario=='success' else 'old'), scenario
        calls=(work/'calls').read_text()
        if scenario in ['corrupt','unsupported']:
            assert '\nrestart ' not in calls and '\nstop ' not in calls, calls
        if scenario=='unhealthy':
            assert 'restoring the previous' in result.stderr and '\nstart ' in calls, result.stderr
        assert ('SELFVPS_HYSTERIA_UPDATE_OK' in result.stdout) == (scenario=='success')
        print('PASS:', scenario)
