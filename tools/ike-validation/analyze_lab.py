"""Analyze ARM guest records, not the host emulator's RSS."""
import json
from pathlib import Path
import re
import sys

def fields(line):
    return dict(re.findall(r'([a-z_]+)=([^\s]+)', line))

def analyze(text):
    if 'MEASUREMENT_METHOD smaps_read_to_eof=1' not in text:
        raise ValueError('old/incomplete smaps sampling method')
    if 'LAB_CLIENT_COMPLETE' not in text or 'ESTABLISHED, IKEv2' not in text or 'INSTALLED, TUNNEL' not in text:
        raise ValueError('protocol acceptance is incomplete')
    if '100 packets transmitted, 100 packets received, 0% packet loss' not in text:
        raise ValueError('bidirectional encrypted traffic acceptance missing')
    for bad in ['LAB_CLIENT_FAILED', 'TARGET_GONE', 'STATUS_INVALID', 'SMAPS_READ_FAILED', 'Kernel panic', 'Out of memory', 'Killed process', 'Oops:', 'BUG:', 'GIC CPU mask not found']:
        if bad in text: raise ValueError('invalid measurement: '+bad)
    phases, cli, smaps = {}, [], {}
    for line in text.splitlines():
        if line.startswith('MEMORY_SUMMARY '):
            d = fields(line); label = d.pop('label'); phases[label] = {k:int(v) for k,v in d.items()}
        elif line.startswith('CLI_MEMORY '):
            d = fields(line)
            if d['rc'] != '0': raise ValueError('CLI operation failed: '+d['label'])
            cli.append({'label':d['label'], **{k:int(v) for k,v in d.items() if k != 'label'}})
        elif line.startswith('SMAPS '):
            d = fields(line); label = d.pop('label'); smaps[label] = {k:int(v) for k,v in d.items()}
    if set(phases) != {'IDLE','NEGOTIATION','TRAFFIC','REKEY','ESTABLISHED'} or not cli:
        raise ValueError('incomplete memory phases')
    peak = max(max(v['lifetime_hwm_kib'], v.get('sampled_smaps_rss_kib', 0)) for v in phases.values())
    cli_peak = max(v['maxrss_kib'] for v in cli)
    return {'status':'passed', 'daemon_peak_rss_kib':peak, 'cli_peak_rss_kib':cli_peak,
            'conservative_daemon_plus_cli_kib':peak+cli_peak,
            'overlap_is_upper_estimate_not_simultaneous_sample':True,
            'phases':phases, 'cli':cli, 'smaps_at_phase_end':smaps}

if __name__ == '__main__':
    print(json.dumps({str(p):analyze(Path(p).read_text(errors='replace')) for p in sys.argv[1:]},indent=2))
