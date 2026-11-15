#!/usr/bin/env python3
"""Read-only production sources; synthetic incoming PDU Go/C comparison."""
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import tempfile
import shlex

ROOT = Path(__file__).resolve().parents[2]
DEPS = Path(os.environ.get('QDC_DEPS', str(Path.home() / '.cache/qdc507-native-c/host/prefix')))
C_ORACLE = r'''#include "qdc_domain.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void h(qdc_buf *o,const qdc_buf *b){for(size_t i=0;i<b->len;i++)qdc_buf_printf(o,"%02x",b->data[i]);}
int main(void){char *line=NULL;size_t cap=0;ssize_t n;while((n=getline(&line,&cap,stdin))>=0){qdc_error e={0};qdc_part p={0};yyjson_doc *d=qdc_json_read(line,n,&e);qdc_slice s=qdc_json_slice(yyjson_doc_get_root(d));int rc=qdc_pdu_deliver(s,&p,&e);if(rc)puts("{\"ok\":false}");else{qdc_buf a={0},b={0};h(&a,&p.sender);h(&b,&p.body);printf("{\"ok\":true,\"sender\":\"%s\",\"body\":\"%s\",\"time\":\"%s\",\"unix\":%lld,\"ref\":%d,\"total\":%d,\"seq\":%d}\n",a.data?(char*)a.data:"",b.data?(char*)b.data:"",p.local_time,(long long)p.timestamp,p.ref,p.total,p.seq);qdc_buf_free(&a);qdc_buf_free(&b);}qdc_part_free(&p);yyjson_doc_free(d);}free(line);}
'''
GO_ORACLE = r'''package main
import("encoding/json";"encoding/hex";"os";"bufio")
func main(){s:=bufio.NewScanner(os.Stdin);w:=json.NewEncoder(os.Stdout);for s.Scan(){var in string;json.Unmarshal(s.Bytes(),&in);p,e:=parseDeliver(in);if e!=nil{w.Encode(map[string]interface{}{"ok":false});continue};w.Encode(map[string]interface{}{"ok":true,"sender":hex.EncodeToString([]byte(p.sender)),"body":hex.EncodeToString([]byte(p.body)),"time":p.t.Format("2006-01-02 15:04:05"),"unix":p.t.Unix(),"ref":p.ref,"total":p.total,"seq":p.seq})}}
'''

def main():
    rng = random.Random(506)
    cases = []
    for i in range(6000):
        fo = rng.choice([0, 64, 2, 4])
        oa = rng.randrange(0, 25)
        toa = rng.choice([0x81, 0x91, 0xD0])
        address = rng.randbytes((oa + 1) // 2)
        scts = rng.randbytes(7)
        dcs = rng.randrange(0, 16)
        ud = rng.randbytes(rng.randrange(0, 180))
        udl = rng.randrange(0, 256)
        if fo == 64 and i % 2:
            ud = bytes([5, 0, 3, rng.randrange(256), rng.randrange(256), rng.randrange(256)]) + ud
        cases.append((bytes([0, fo, oa, toa]) + address + bytes([0, dcs]) + scts + bytes([udl]) + ud).hex())
    wire = ''.join(json.dumps(c) + '\n' for c in cases).encode()
    with tempfile.TemporaryDirectory(prefix='qdc-spec-pdu-') as td:
        work = Path(td)
        (work / 'main.c').write_text(C_ORACLE)
        subprocess.run(shlex.split(os.environ.get('CC','cc')) + (['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.environ.get('QDC_SANITIZE') == '1' else []) + ['-std=c11', '-D_DEFAULT_SOURCE', '-D_DARWIN_C_SOURCE', '-O1',
                        '-I' + str(ROOT / 'native-c/include'), '-I' + str(DEPS / 'include'),
                        str(work / 'main.c'), str(ROOT / 'native-c/src/pdu.c'), str(ROOT / 'native-c/src/common.c'),
                        '-L' + str(DEPS / 'lib'), '-lyyjson', '-pthread', '-o', str(work / 'c')], check=True)
        (work / 'pdu.go').write_bytes((ROOT / 'pdu.go').read_bytes())
        (work / 'main.go').write_text(GO_ORACLE)
        env = os.environ.copy()
        env.pop('GOROOT', None)
        if os.environ.get('QDC_GO_ROOT'): env['GOROOT'] = os.environ['QDC_GO_ROOT']
        env.update(GOENV='off', GOTOOLCHAIN='local', GOPROXY='off', GOSUMDB='off')
        subprocess.run([os.environ.get('QDC_GO','go'), 'build', '-o', str(work / 'go'), str(work / 'pdu.go'), str(work / 'main.go')], check=True, env=env)
        left = subprocess.run([str(work / 'c')], input=wire, stdout=subprocess.PIPE, check=True).stdout.splitlines()
        right = subprocess.run([str(work / 'go')], input=wire, stdout=subprocess.PIPE, check=True).stdout.splitlines()
    assert len(left) == len(right) == len(cases)
    mismatches = [(i, cases[i], a.decode(), b.decode()) for i, (a, b) in enumerate(zip(left, right)) if json.loads(a) != json.loads(b)]
    result = {'cases': len(cases), 'mismatches': len(mismatches), 'seed': 506,
              'fixture_sha256': hashlib.sha256(wire).hexdigest(),
              'go_source_sha256': hashlib.sha256((ROOT / 'pdu.go').read_bytes()).hexdigest(),
              'c_source_sha256': hashlib.sha256((ROOT / 'native-c/src/pdu.c').read_bytes()).hexdigest()}
    print(json.dumps(result, sort_keys=True))
    for mismatch in mismatches[:3]:
        print(json.dumps(mismatch))
    assert not mismatches

if __name__ == '__main__':
    main()
