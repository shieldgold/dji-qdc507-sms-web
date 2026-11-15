"""Evidence shared by the C and Go comparisons; does not change runtime guards."""
import re


def validate_preflight(text, gate):
    text = text.lstrip('\x00')
    if gate not in ('sms', 'coexist'):
        raise ValueError('invalid entropy preflight gate')
    if 'QDC_ENTROPY_FAILED' in text or 'QDC_ENTROPY_PREFLIGHT_FAILED' in text:
        raise ValueError('entropy preflight failed')
    starts = list(re.finditer(
        r'^QDC_ENTROPY_PREFLIGHT gate=(sms|coexist) timeout_ms=90000 '
        r'admission_kib=8192 absolute_floor_kib=6144 max_drop_kib=3072\r?$', text, re.M))
    ready = list(re.finditer(
        r'^QDC_ENTROPY_READY elapsed_ms=(\d+) getrandom_return=32 errno=0 '
        r'attempts=(\d+) min_available_kib=(\d+) reason=ready\r?$', text, re.M))
    if len(starts) != 1 or len(ready) != 1 or starts[0][1] != gate or starts[0].start() >= ready[0].start():
        raise ValueError('missing or inconsistent entropy preflight evidence')
    elapsed, attempts, minimum = map(int, ready[0].groups())
    if elapsed > 90000 or attempts < 1 or minimum < 6144:
        raise ValueError('entropy preflight exceeded its limits')
    return {'status': 'ready', 'gate': gate, 'elapsed_ms': elapsed,
            'getrandom_return': 32, 'errno': 0, 'attempts': attempts,
            'min_available_kib': minimum, 'timeout_ms': 90000}
