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
