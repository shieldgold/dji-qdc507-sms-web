"""Complete smsweb + original guarded charon on a private two-VM link."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time

def validate(text):
    for marker in ('SMS_COMBO_REAL_SMS_STARTED','SMS_COMBO_FUNCTIONAL_PASS',
                   'SMS_COMBO_CLOCK_ORDER_OK','SMS_COMBO_NETWORK_UNCHANGED','SMS_COMBO_VPN_CLEANED',
                   'COEXIST_POST_SMS_OK same_identity=1 http_status=200',
                   'COEXIST_COMPLETE rc=0 children_reaped=1','SMS_COMBO_PASS'):
        if marker not in text:raise ValueError('missing evidence: '+marker)
    if any(x in text for x in ('STOP memory','SMS_COMBO_HTTP_FAILED','COEXIST_VM_FAILED','Kernel panic','SIGSEGV','RECOVERY_REQUIRED')):
        raise ValueError('runtime/guard failure')
    match=re.search(r'COEXIST_TRAFFIC_COMPLETE seconds=([0-9.]+) ping_pairs=10 http_success=10',text)
    if not match or float(match[1])<30:raise ValueError('no complete 30-second window')
    pairs=list(map(int,re.findall(r'COEXIST_PAIR index=(\d+) ping=1 http_status=200',text)))
    if pairs!=list(range(1,11)):raise ValueError('not ten distinct encrypted ping/HTTP pairs')
    fake=re.search(r'SMS_COMBO_FAKE commands=(\d+) polls=(\d+) submitted=(\d+) cancelled=(\d+) unlocks=(\d+)',text)
    if not fake or int(fake[2])<10 or int(fake[3])!=1:raise ValueError('no real SMS polling/synthetic submission evidence')
    return {'status':'passed','device_executed':False,'seconds':float(match[1]),'ping_pairs':10,'http_pairs':10,
            'real_complete_smsweb':True,'synthetic_modem_only':True,
            'fake_modem':dict(zip(('commands','polls','submitted','cancelled','unlocks'),map(int,fake.groups())))}

def command(kernel,initrd,serial,role,port):
    if role not in ('server','client') or not 1<=port<=65535:raise ValueError('invalid private link')
    network=('listen' if role=='server' else 'connect')+'=127.0.0.1:'+str(port)
    return ['qemu-system-arm','-M','virt-5.2','-cpu','cortex-a7','-smp','2','-m','48M',
            '-nodefaults','-display','none','-monitor','none','-no-reboot','-serial','file:'+str(serial),
            '-global','virtio-mmio.force-legacy=true','-object','rng-random,filename=/dev/urandom,id=rng0',
            '-device','virtio-rng-device,rng=rng0','-netdev','socket,id=private,'+network,
            '-device','virtio-net-device,netdev=private,mac=52:54:00:12:36:'+('01' if role=='server' else '02'),
            '-kernel',str(kernel),'-initrd',str(initrd),'-append',
            'console=ttyAMA0 loglevel=3 panic=1 maxcpus=1 rdinit=/init rng_core.default_quality=1000 '+
            'qdc.coexist.lab=1 qdc.sms.combo=1 qdc.case=normal_30s qdc.role='+role+' qdc.epoch='+str(int(time.time()))]

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def prepare_pki(directory):
    marker=directory/'generated.json'
    if directory.exists():
        if not marker.is_file():raise ValueError('PKI directory was not created by this lab')
        for name in ('ca','client','server'):
            subprocess.run(['openssl','x509','-in',str(directory/(name+'.pem')),'-checkend','120','-noout'],
                           check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        return
    directory.mkdir(mode=0o700,parents=True,exist_ok=False)
    def run(*args):subprocess.run(['openssl',*map(str,args)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    run('genrsa','-out',directory/'ca.key','2048')
    run('req','-new','-x509','-sha256','-days','7','-key',directory/'ca.key','-subj','/CN=QDC SMS COMBO SYNTHETIC CA','-out',directory/'ca.pem',
        '-addext','basicConstraints=critical,CA:TRUE','-addext','keyUsage=critical,keyCertSign,cRLSign')
    for name in ('client','server'):
        run('genrsa','-out',directory/(name+'.key'),'2048')
        run('req','-new','-sha256','-key',directory/(name+'.key'),'-subj','/CN='+name+'.lab','-out',directory/(name+'.csr'))
        extension=directory/(name+'.ext')
        extension.write_text('basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=clientAuth,serverAuth\nsubjectAltName=DNS:'+name+'.lab\n')
        run('x509','-req','-sha256','-days','7','-in',directory/(name+'.csr'),'-CA',directory/'ca.pem','-CAkey',directory/'ca.key',
            '-CAcreateserial','-extfile',extension,'-out',directory/(name+'.pem'))
    marker.write_text(json.dumps({'purpose':'computer-only synthetic SMS/IKE lab','rsa_bits':2048,'valid_days':7,'created_epoch':int(time.time())},indent=2)+'\n')

def main():
    os.umask(0o077)
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('base',type=Path);p.add_argument('--binary',type=Path,required=True)
    p.add_argument('--label',required=True);p.add_argument('--attempt',type=int,required=True)
    p.add_argument('--pki',type=Path,required=True)
    args=p.parse_args()
    if args.attempt<1 or not re.fullmatch(r'[A-Za-z0-9_-]{1,40}',args.label):p.error('invalid label/attempt')
    base=args.base.resolve();binary=args.binary.resolve();pki=args.pki.resolve()
    source=Path(__file__).resolve().parent;tools=source.parents[1]
    guard=base/'coexist-build/qdc-ike-guard';kernel=base/'runtime-build/lab-only-runtime-zImage'
    logs=base/'sms-opt-build/combined-logs'/f'{args.label}-{args.attempt}'
    logs.mkdir(mode=0o700,parents=True,exist_ok=False)
    config=tools/'ike-validation/runtime/real.conf'
    if not re.search(r'^\s*threads\s*=\s*16\s*$',config.read_text(),re.M):raise ValueError('original 16-thread configuration changed')
    prepare_pki(pki)
    build=['docker','run','--rm','--network','none','--platform','linux/amd64',
           '-v',str(source.parent)+':/sms:ro','-v',str(tools)+':/tools:ro',
           '-v',str(binary)+':/input/smsweb:ro','-v',str(guard)+':/input/guard:ro',
           '-v',str(base/'output')+':/ike:ro','-v',str(pki)+':/pki:ro',
           '-v',str(logs)+':/out','-v','qdc507-ike-build-v1:/work',
           'qdc507-ike-builder:local','sh','/sms/combined/build.sh']
    (logs/'build-command.json').write_text(json.dumps(build,indent=2)+'\n')
    with (logs/'build.log').open('wb') as output:subprocess.run(build,check=True,stdout=output,stderr=subprocess.STDOUT)
    initrd=logs/'lab-only-sms-combo-rootfs.cpio.gz'
    paths={'smsweb':binary,'guard':guard,'kernel':kernel,'charon':base/'output/charon','swanctl':base/'output/swanctl','config':config,'initrd':initrd}
    result={'status':'failed','device_executed':False,'artifact_sha256':{k:sha(v) for k,v in paths.items()},
            'pki_cert_sha256':{n:sha(pki/(n+'.pem')) for n in ('ca','client','server')},
            'fixture_source_sha256':{str(f.relative_to(source.parent)):sha(f) for f in sorted(source.rglob('*')) if f.is_file() and '__pycache__' not in str(f)},
            'shared_fixture_source_sha256':{name:sha(source.parent/name) for name in ('fake_modem.c','fake_modem.h','http_metadata.c','http_metadata.h')},
            'metadata_helper_source_sha256':sha(tools/'ike-validation/runtime/coexist-vm-fixture.c'),
            'memory_mib':48,'charon_threads':16,'gogc':20,'soft_go_limit_mib':12,
            'admission_kib':8192,'absolute_floor_kib':6144,'max_drop_kib':3072,
            'network':'QEMU socket pair on 127.0.0.1 only; no WAN/USB; exact /32 VPN route'}
    with socket.socket() as reserve:reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
    processes=[]
    try:
        for role in ('server','client'):
            cmd=command(kernel,initrd,logs/(role+'.log'),role,port)
            (logs/(role+'-command.json')).write_text(json.dumps(cmd,indent=2)+'\n')
            error=(logs/(role+'-qemu.log')).open('wb')
            proc=subprocess.Popen(cmd,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=error);processes.append((proc,error))
            deadline=time.monotonic()+(40 if role=='server' else 125)
            while time.monotonic()<deadline:
                log=(logs/(role+'.log')).read_text(errors='replace') if (logs/(role+'.log')).exists() else ''
                wanted='COEXIST_VM_SERVER_READY' if role=='server' else 'SMS_COMBO_PASS'
                if wanted in log:break
                if proc.poll() is not None or 'COEXIST_VM_FAILED' in log or 'Kernel panic' in log:raise RuntimeError(role+' guest failed')
                time.sleep(.25)
            else:raise TimeoutError(role+' guest deadline')
        result.update(validate(log))
    except Exception as error:
        result['error']=str(error)
        client=logs/'client.log'
        if client.exists():result['stop_markers']=re.findall(r'^(?:STOP|PREFLIGHT_MEMORY_REFUSED|MEMORY_SUMMARY|COEXIST_COMPLETE)[^\n]*',client.read_text(errors='replace'),re.M)
        raise
    finally:
        for proc,error in reversed(processes):
            if proc.poll() is None:
                proc.terminate()
                try:proc.wait(timeout=3)
                except subprocess.TimeoutExpired:proc.kill();proc.wait()
            error.close()
        (logs/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'status':result['status'],'logs':str(logs),'artifact_sha256':result['artifact_sha256'],'fake_modem':result['fake_modem']}))

if __name__=='__main__':main()
