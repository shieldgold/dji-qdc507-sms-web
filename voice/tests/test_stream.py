import subprocess,time,threading
p=subprocess.Popen(['/tmp/pcm-test','--stdio','--no-mixers','--library','/tmp/libfake.so'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
received=[]
t=threading.Thread(target=lambda:received.append(p.stdout.read()));t.start()
try:
 for _ in range(5):
  p.stdin.write(b'\x01'*319);p.stdin.flush();time.sleep(.03)
 p.stdin.close()
 p.wait(timeout=6);t.join(timeout=2)
except Exception:
 p.kill();p.wait();raise
assert p.returncode==0,p.stderr.read().decode()
assert received and len(received[0])>=320,'no downlink'
assert set(received[0])=={0x12},'vendor logs corrupted audio'
assert len(received[0])%2==0,'unaligned PCM'
print('PASS: duplex PCM, odd input fragments, clean stdout, bounded EOF shutdown')

# The module's idle PCM endpoint can return silence without blocking. It must
# never flood the network or make the iOS playback queue discard real frames.
import os
p=subprocess.Popen(['/tmp/pcm-test','--stdio','--no-mixers','--library','/tmp/libfake.so'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,env={**os.environ,'FAKE_AUDIO_UNPACED':'1'})
count=[0]
def drain():
 while True:
  data=p.stdout.read(4096)
  if not data:break
  count[0]+=len(data)
t=threading.Thread(target=drain);t.start();started=time.monotonic()
time.sleep(.3);p.stdin.close();p.wait(timeout=6);t.join(timeout=2)
elapsed=time.monotonic()-started
assert p.returncode==0,p.returncode
assert count[0] <= elapsed*20000+640,('unpaced PCM flood',count[0],elapsed)
print('PASS: nonblocking vendor PCM stays bounded at the sample clock')
