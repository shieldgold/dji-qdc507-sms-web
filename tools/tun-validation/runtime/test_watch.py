import os, subprocess, tempfile, select
from pathlib import Path

def run(send_go):
    with tempfile.TemporaryDirectory() as d:
        marker=Path(d)/'executed'
        readfd, writefd=os.pipe()
        env=dict(os.environ,QDC_TEST_LOG_FD=str(readfd))
        p=subprocess.Popen(['/out/test-watch','/usr/bin/touch',str(marker)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,pass_fds=(readfd,),env=env)
        try:
            assert select.select([p.stdout],[],[],2)[0], 'no READY before timeout'
            first=p.stdout.readline()
            assert b'__READY_TEST__' in first, (first,p.poll())
            assert not marker.exists(), 'command ran before GO'
            out,_=p.communicate(b'GO\n' if send_go else b'',timeout=4)
            assert marker.exists()==send_go, (out,p.returncode)
            assert (p.returncode==0)==send_go, (out,p.returncode)
            if send_go: assert b'watch_rss_kb=' in out, out
        finally:
            if p.poll() is None: p.kill();p.wait()
            os.close(readfd);os.close(writefd)
run(True)
run(False)
print('watcher gate checks: executes after GO; EOF never executes')
