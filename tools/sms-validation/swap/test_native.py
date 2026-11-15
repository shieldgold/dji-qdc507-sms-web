"""Real bind mounts/signals in a disposable Linux container, never on a device."""
import hashlib
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time
import unittest
import uuid

BIN = Path(os.environ['SMS_SWAP_HELPER'])
FIXTURES = Path(os.environ['SMS_SWAP_FIXTURES'])
TARGET = Path('/data/smsweb/smsweb')

def wait_for(check, seconds=5):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        value = check()
        if value:
            return value
        time.sleep(.02)
    raise AssertionError('disposable fixture did not reach expected state')

def identity(pid):
    try:
        text = Path(f'/proc/{pid}/stat').read_text()
        fields = text[text.rfind(')')+2:].split()
        return fields[0], int(fields[1]), int(fields[19])
    except FileNotFoundError:
        return None

def children(pid):
    try:
        return [int(x) for x in Path(f'/proc/{pid}/task/{pid}/children').read_text().split()]
    except FileNotFoundError:
        return []

def mounted():
    return any(line.split()[4] == str(TARGET) for line in Path('/proc/self/mountinfo').read_text().splitlines())

class NativeSwapTests(unittest.TestCase):
    def setUp(self):
        self.run = 'LAB' + uuid.uuid4().hex[:12]
        self.ram = Path('/run/qdc-sms-' + self.run)
        self.data = Path('/data/qdc-sms-' + self.run)
        self.clock = Path('/run/qdc-tun-' + self.run)
        self.ram.mkdir(mode=0o700)
        self.clock.mkdir(mode=0o700)
        Path('/data/smsweb').mkdir(mode=0o700, exist_ok=True)
        os.chmod('/data/smsweb',0o700)
        shutil.copy2(FIXTURES/'original',TARGET)
        os.chmod(TARGET,0o700)
        self.original_hash = hashlib.sha256(TARGET.read_bytes()).hexdigest()
        Path('/data/smsweb/start.sh').write_text('fixture supervisor; unchanged\n')
        self.script_hash = hashlib.sha256(Path('/data/smsweb/start.sh').read_bytes()).hexdigest()
        self.helper = self.ram/'qdc-sms-swap'
        shutil.copy2(BIN,self.helper); self.helper.chmod(0o500)
        self.sup = self.guard = self.hold = None

    def tearDown(self):
        for proc in (self.hold,self.guard):
            if proc and proc.poll() is None:
                proc.kill(); proc.wait(timeout=3)
        if self.sup:
            try: os.killpg(self.sup.pid,signal.SIGKILL)
            except ProcessLookupError: pass
            self.sup.wait(timeout=3)
        while mounted():
            subprocess.run(['umount',str(TARGET)],check=True)
        for p in (self.ram,self.data,self.clock):
            if p.exists(): shutil.rmtree(p)

    def copy(self):
        binary = (FIXTURES/'optimized').read_bytes()
        result = subprocess.run([str(self.helper),'copy',self.run,str(len(binary))],input=binary,capture_output=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn(b'COPY_READY',result.stdout)
        self.assertEqual((self.data/'smsweb').read_bytes(),binary)
        self.assertEqual((self.data.stat().st_mode & 0o777),0o700)
        self.assertEqual(((self.data/'smsweb').stat().st_mode & 0o777),0o500)

    def bind_control(self, clock_fail=False):
        self.copy()
        shutil.copy2(FIXTURES/'supervisor','/bin/busybox')
        self.sup = subprocess.Popen(['/bin/busybox','/bin/sh','/data/smsweb/start.sh','supervise'],start_new_session=True)
        old = wait_for(lambda: next((p for p in children(self.sup.pid) if identity(p) and identity(p)[0]!='Z'),None))
        os.kill(self.sup.pid,signal.SIGSTOP)
        wait_for(lambda: identity(self.sup.pid)[0]=='T')
        os.kill(old,signal.SIGTERM)
        wait_for(lambda: not identity(old) or identity(old)[0]=='Z')
        self.sup_start = identity(self.sup.pid)[2]
        self.assertEqual(os.readlink(f'/proc/{self.sup.pid}/exe'),'/bin/busybox','fixture must reproduce the embedded non-usr-merged /bin identity')
        guard_exe=self.clock/'qdc-ike-guard'
        shutil.copy2(FIXTURES/'guardian',guard_exe); guard_exe.chmod(0o500)
        fd=os.open(self.clock/'sms-guard.log',os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
        env=dict(os.environ)
        if clock_fail: env['SMS_SWAP_FIXTURE_CLOCK_FAIL']='1'
        with os.fdopen(fd,'wb') as log:
            self.guard=subprocess.Popen([str(guard_exe),'resume-clock',str(self.sup.pid),str(self.sup_start),'300'],stdout=log,stderr=log,env=env,start_new_session=True)
        wait_for(lambda: 'RESUME_GUARD_READY' in (self.clock/'sms-guard.log').read_text())
        oldst,newst=TARGET.stat(),(self.data/'smsweb').stat()
        control=(f'QDC_SMS_SWAP_V1\nRUN {self.run}\nBOOT {Path("/proc/sys/kernel/random/boot_id").read_text().strip()}\n'
                 f'SUP {self.sup.pid} {self.sup_start}\nORIGINAL {oldst.st_dev} {oldst.st_ino} {oldst.st_size}\n'
                 f'SOURCE {newst.st_dev} {newst.st_ino} {newst.st_size}\nCLOCK {self.guard.pid} {identity(self.guard.pid)[2]}\nSECONDS 300\n')
        path=self.ram/'lease.control'; path.write_text(control); path.chmod(0o600)

    def start_hold(self):
        self.hold=subprocess.Popen([str(self.helper),'hold',self.run],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        def ready():
            if self.hold.poll() is not None:
                out,err=self.hold.communicate()
                log=(self.ram/'swap.log').read_bytes() if (self.ram/'swap.log').exists() else b''
                raise AssertionError(f'lease exited {self.hold.returncode}: {out+err+log!r}')
            return (self.ram/'swap.log').exists() and 'SWAP_READY' in (self.ram/'swap.log').read_text()
        wait_for(ready)
        self.assertTrue(mounted())

    def finish(self):
        self.hold.send_signal(signal.SIGTERM)
        out,err=self.hold.communicate(timeout=12)
        self.assertEqual(self.hold.returncode,0,out+err+(self.ram/'swap.log').read_bytes())
        self.assertFalse(mounted())
        self.assertFalse(self.data.exists())
        self.assertEqual(hashlib.sha256(TARGET.read_bytes()).hexdigest(),self.original_hash)
        self.assertEqual(hashlib.sha256(Path('/data/smsweb/start.sh').read_bytes()).hexdigest(),self.script_hash)
        self.assertIn('SWAP_COMPLETE source_removed=1',(self.ram/'swap.log').read_text())
        wait_for(lambda: identity(self.sup.pid)[0]!='T')

    def test_copy_syncs_private_file_and_refuses_overwrite(self):
        self.copy()
        before=(self.data/'smsweb').read_bytes()
        result=subprocess.run([str(self.helper),'copy',self.run,'3'],input=b'new',capture_output=True)
        self.assertNotEqual(result.returncode,0)
        self.assertEqual((self.data/'smsweb').read_bytes(),before)

    def test_copy_rejects_short_input_and_removes_only_owned_partial(self):
        result=subprocess.run([str(self.helper),'copy',self.run,'10'],input=b'short',capture_output=True)
        self.assertNotEqual(result.returncode,0)
        self.assertFalse(self.data.exists())

    def test_copy_failure_never_removes_replaced_directory(self):
        proc=subprocess.Popen([str(self.helper),'copy',self.run,'10'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        wait_for(lambda: (self.data/'smsweb').exists())
        moved=self.data.with_name(self.data.name+'-moved')
        self.data.rename(moved)
        self.data.mkdir(mode=0o700)
        try:
            out,err=proc.communicate(b'short',timeout=3)
            self.assertNotEqual(proc.returncode,0,out+err)
            self.assertTrue(self.data.is_dir(),'copy failure removed an unknown replacement directory')
        finally:
            if moved.exists(): shutil.rmtree(moved)

    def test_large_copy_exercises_bounded_writeback(self):
        payload=(FIXTURES/'optimized').read_bytes()+b'x'*(6*1024*1024)
        result=subprocess.run([str(self.helper),'copy',self.run,str(len(payload))],input=payload,capture_output=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn(b'fsync=1 private_fadvise=1',result.stdout)
        self.assertEqual(hashlib.sha256((self.data/'smsweb').read_bytes()).digest(),hashlib.sha256(payload).digest())

    def test_storage_refusal_creates_no_files(self):
        result=subprocess.run([str(self.helper),'copy',self.run,str(6*1024*1024)],input=b'',capture_output=True)
        self.assertNotEqual(result.returncode,0)
        self.assertIn(b'COPY_REFUSED storage_requires_full_copy_plus_1MiB=1',result.stdout)
        self.assertFalse(self.data.exists())
        self.assertEqual(hashlib.sha256(TARGET.read_bytes()).hexdigest(),self.original_hash)

    def test_idle_stdin_is_interruptible_without_host_eof(self):
        proc=subprocess.Popen([str(self.helper),'copy',self.run,'10'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            wait_for(lambda: (self.data/'smsweb').exists())
            proc.send_signal(signal.SIGTERM)
            try: proc.wait(timeout=1)
            except subprocess.TimeoutExpired: self.fail('copy remained blocked in read(stdin) after SIGTERM while host pipe stayed open')
            self.assertNotEqual(proc.returncode,0)
            self.assertFalse(self.data.exists())
        finally:
            if proc.poll() is None: proc.kill(); proc.wait(timeout=2)
            for stream in (proc.stdin,proc.stdout,proc.stderr): stream.close()

    def test_idle_stdin_expires_at_real_monotonic_deadline(self):
        proc=subprocess.Popen([str(self.helper),'copy',self.run,'10'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        began=time.monotonic()
        try:
            proc.wait(timeout=94)
            elapsed=time.monotonic()-began
            self.assertGreaterEqual(elapsed,89)
            self.assertLess(elapsed,94)
            self.assertNotEqual(proc.returncode,0)
            self.assertIn(b'monotonic_deadline_seconds=90',proc.stdout.read())
            self.assertFalse(self.data.exists())
        finally:
            if proc.poll() is None: proc.kill(); proc.wait(timeout=2)
            for stream in (proc.stdin,proc.stdout,proc.stderr): stream.close()

    def test_memory_floor_before_and_after_private_sync(self):
        payload=(FIXTURES/'optimized').read_bytes()+b'x'*(128*1024)
        for high_calls in ('0','1','2'):
            env=dict(os.environ,SMS_SWAP_TEST_LOW_AFTER=high_calls)
            result=subprocess.run([str(self.helper),'copy',self.run,str(len(payload))],input=payload,capture_output=True,env=env)
            self.assertNotEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn(b'memory_floor_kib=6144',result.stdout)
            self.assertFalse(self.data.exists())

    def test_early_rollback_restores_before_clock_handoff(self):
        self.bind_control(); self.start_hold(); self.finish()
        text=(self.clock/'sms-guard.log').read_text()
        self.assertLess(text.index('CLOCK_RESTORED'),text.index('RESUME_SENT'))

    def test_running_optimized_child_is_stopped_then_original_restored(self):
        self.bind_control(); self.start_hold()
        self.guard.send_signal(signal.SIGTERM); self.guard.wait(timeout=3)
        source=(self.data/'smsweb').stat()
        def optimized_child():
            for pid in children(self.sup.pid):
                try: st=Path(f'/proc/{pid}/exe').stat()
                except FileNotFoundError: continue
                if (st.st_dev,st.st_ino)==(source.st_dev,source.st_ino): return pid
            return None
        pid=wait_for(optimized_child)
        self.finish()
        self.assertTrue(not identity(pid) or identity(pid)[0]=='Z')

    def test_unknown_overlay_is_retained_without_cont(self):
        self.bind_control(); self.start_hold()
        foreign=self.ram/'foreign'; foreign.write_bytes(b'unknown-resource')
        subprocess.run(['mount','--bind',str(foreign),str(TARGET)],check=True)
        self.hold.send_signal(signal.SIGTERM); self.hold.communicate(timeout=12)
        self.assertNotEqual(self.hold.returncode,0)
        self.assertEqual(TARGET.read_bytes(),b'unknown-resource')
        self.assertTrue(self.data.exists())
        self.assertEqual(identity(self.sup.pid)[0],'T')
        self.assertIn('RECOVERY_REQUIRED',(self.ram/'swap.log').read_text())

    def test_missing_clock_proof_does_not_authorize_cont(self):
        self.bind_control(clock_fail=True); self.start_hold()
        self.hold.send_signal(signal.SIGTERM); self.hold.communicate(timeout=12)
        self.assertNotEqual(self.hold.returncode,0)
        self.assertFalse(mounted())
        self.assertEqual(identity(self.sup.pid)[0],'T')
        self.assertIn('RECOVERY_REQUIRED',(self.ram/'swap.log').read_text())

if __name__=='__main__': unittest.main()
