import subprocess
r=subprocess.run(['/out/probe-native','selftest'],capture_output=True,text=True)
assert r.returncode==0 and 'SELFTEST_OK' in r.stdout,(r.returncode,r.stdout,r.stderr)
for args in [['bogus'],['features','/etc/passwd'],['create','/dev/qdc-tun-test','../escape']]:
    r=subprocess.run(['/out/probe-native']+args,capture_output=True,text=True)
    assert r.returncode==64,(args,r.returncode)
print('probe checks: CPU selftest and invalid requests rejected before device access')
