#!/usr/bin/env python3
"""Run the real LVGL9.6 timer-policy regression using a matched host archive."""
from pathlib import Path
import argparse,hashlib,json,subprocess
ROOT=Path(__file__).resolve().parent
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--lvgl',type=Path,default=Path('/workspace/work/pr818-upstream-tests96/test/build-plain/lvgl-src'))
p.add_argument('--archive',type=Path,default=Path('/workspace/work/pr818-upstream-tests96/test/transform-build/oracle.a'))
a=p.parse_args()
flags=['cc','-O2','-Wall','-Wextra','-Werror','-DLV_CONF_INCLUDE_SIMPLE','-DLV_DRAW_TRANSFORM_USE_MATRIX=0','-DLV_USE_OS=0','-DESP_LVGL_ADAPTER_OS_MUTEX_CLEANUP=1','-I'+str(ROOT),'-I'+str(a.lvgl)]
cmd=flags+[str(ROOT/'timer_policy.c'),str(a.archive),'-Wl,--wrap=lv_draw_buf_copy','-lm','-o',str(ROOT/'timer_policy')]
subprocess.run(cmd,check=True)
r=subprocess.run([str(ROOT/'timer_policy')],capture_output=True,text=True)
(ROOT/'policy.log').write_text(r.stdout+r.stderr)
if r.returncode:raise RuntimeError(r.stdout+r.stderr)
rows=[json.loads(line) for line in r.stdout.splitlines() if line.startswith('{')]
(ROOT/'policy-results.json').write_text(json.dumps({'host':True,'lvgl':'9.6.0','archive_sha256':hashlib.sha256(a.archive.read_bytes()).hexdigest(),'source_sha256':hashlib.sha256((ROOT/'timer_policy.c').read_bytes()).hexdigest(),'compile_command':cmd,'results':rows,'note':'One controlled during-refresh invalidation; real LVGL timerhandler and DIRECT buffers, no adapter/RTOS/PPA. This validates timerbehavior, not hardware speed.'},indent=2)+'\n')
print(r.stdout)
