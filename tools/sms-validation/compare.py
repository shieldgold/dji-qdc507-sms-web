"""Compare two successful, identical-fixture VM runs; no device inference."""
import argparse
import json
from pathlib import Path

def compare(before,after):
    if before.get('status')!='passed' or after.get('status')!='passed':
        raise ValueError('both experiments must pass')
    fixed=('scenario','memory_mib','gomaxprocs','gogc','soft_go_memory_limit_mib',
           'poll_seconds','kernel_sha256','fixture_sha256','fixture_source_sha256')
    for key in fixed:
        if key not in before or before[key]!=after.get(key):
            raise ValueError('experiment configuration differs: '+key)
    phases={}
    for phase in ('startup','poll','http','pressure','released'):
        a,b=before['samples'][phase],after['samples'][phase]
        phases[phase]={}
        for metric in ('rss','pss','anonymous'):
            k=metric+'_kib'
            phases[phase].update({metric+'_before_kib':a[k],metric+'_after_kib':b[k],
                                  metric+'_delta_kib':b[k]-a[k]})
    return {'device_executed':False,'scenario':before['scenario'],
            'before_binary_sha256':before['binary_sha256'],
            'after_binary_sha256':after['binary_sha256'],
            'fixture_sha256':before['fixture_sha256'],'phases':phases,
            'limitation':'Same generic ARM VM only; sampled RSS is not a continuous peak or device acceptance.'}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before',type=Path);parser.add_argument('after',type=Path)
    args=parser.parse_args()
    print(json.dumps(compare(json.loads(args.before.read_text()),json.loads(args.after.read_text())),indent=2))

if __name__=='__main__':main()
