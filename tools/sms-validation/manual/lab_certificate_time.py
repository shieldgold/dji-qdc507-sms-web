"""Choose a fixed valid time for disposable offline certificates only."""
from datetime import datetime, timezone
import subprocess


def choose_epoch(windows):
    if not windows or any(start >= end for start, end in windows):
        raise ValueError('nonempty valid certificate windows required')
    epoch = max(start for start, _ in windows) + 60
    if epoch + 5400 >= min(end for _, end in windows):
        raise ValueError('lab certificates need a common 90-minute valid window')
    return epoch


def certificate_epoch(paths):
    windows = []
    for path in paths:
        output = subprocess.check_output(['openssl', 'x509', '-in', str(path), '-noout', '-dates'], text=True)
        fields = dict(line.split('=', 1) for line in output.splitlines())
        if set(fields) != {'notBefore', 'notAfter'}:
            raise ValueError('certificate validity fields required')
        windows.append(tuple(int(datetime.strptime(fields[key], '%b %d %H:%M:%S %Y %Z').replace(tzinfo=timezone.utc).timestamp())
                             for key in ('notBefore', 'notAfter')))
    return choose_epoch(windows)
