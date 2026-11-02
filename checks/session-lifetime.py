#!/usr/bin/env python3
"""Check the session lifetime rule and the named admission refusals without a build.

An OS image's session lifetime derives from its application manifest (tools/session_lifetime.py). The OS session
decoder (apps/os/session.c) refuses a manifest whose run deadline ends later than the lifetime minus the return reserve.
Cases:
  rule        derive() never returns a lifetime that check() refuses, at every boundary of run_us and return_reserve_us
              (exactly at the decoder's bound, one microsecond over, a reserve of the whole lifetime, a lifetime over the limit);
  manifests   every application manifest in the tree derives a lifetime inside 1..MAX_SECONDS that the decoder admits, and
              the set includes a manifest that the earlier fixed 60 s lifetime refused (a run over 55 s);
  limits      the compiled lifetime range in include/a733_board_application.h, the host's watchdog-marker bound and the
              thermal decoder's row bound all come from the same MAX_SECONDS;
  table       the refusal terms and sub-terms in include/a733_admission_refusal.h are the ones tools/a733-crumbs.py decodes,
              every detail fits the 12-bit breadcrumb detail, and a breadcrumb word of an EL3 stop names its term;
  sources     no source outside the lifetime derivation compiles a lifetime figure (a literal 60 in the consumers of
              A733_BOARD_SUPERVISED_SECONDS).
Usage: session-lifetime.py --output DIR
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['tools/session_lifetime.py', 'include/a733_admission_refusal.h', 'include/a733_board_application.h',
                'tools/a733-crumbs.py', 'apps/os/thermal-report.py', 'apps/os/host.py', 'apps/os/thermal.c', 'apps/os/session.c',
                'apps/large-region-test/manifest-return.json', 'apps/storage-write-test/manifest.json']


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def rule(lt):
    cases = 0
    for run_us in (0, 1, 2_000_000, 55_000_000, 59_999_999, 60_000_000, 120_000_000, 300_000_000, 3_000_000_000):
        for reserve_us in (0, 1, 5_000_000, 30_000_000):
            if lt.required_seconds(run_us, reserve_us) > lt.MAX_SECONDS:
                harness.refused(lambda: lt.derive({'deadlines': {'run_us': run_us, 'return_reserve_us': reserve_us}}), 'longest session',
                                lt.LifetimeError)
                cases += 1
                continue
            seconds = lt.derive({'deadlines': {'run_us': run_us, 'return_reserve_us': reserve_us}})
            lt.check(run_us, reserve_us, seconds)            # the derived lifetime is admitted
            assert seconds >= lt.FLOOR_SECONDS and seconds * 1_000_000 >= run_us + reserve_us + (lt.STARTUP_SECONDS + lt.SETUP_SECONDS) * 1_000_000 - 999_999
            cases += 1
    # The decoder's bound, exactly: lifetime minus reserve admits, one microsecond more is refused with its term.
    lt.check(55_000_000, 5_000_000, 60)
    error = harness.refused(lambda: lt.check(55_000_001, 5_000_000, 60), 'exceeds the 60 s session lifetime', lt.LifetimeError)
    assert 'by 1 us' in error and 'at least 61 s' in error, error
    for call, term in ((lambda: lt.check(1, 60_000_000, 60), 'return_reserve'), (lambda: lt.check(1, 5_000_000, 0), 'lifetime_range'),
                       (lambda: lt.check(1, 5_000_000, lt.MAX_SECONDS + 1), 'lifetime_range'),
                       (lambda: lt.check(61_000_000, 5_000_000, 60), 'run_deadline')):
        try:
            call()
        except lt.LifetimeError as exc:
            assert exc.term == term, (exc.term, term)
        else:
            raise AssertionError(term)
    # A five-minute test declares run_us and gets a session that holds it.
    assert lt.derive({'deadlines': {'run_us': 300_000_000, 'return_reserve_us': 5_000_000}}) == 310
    return cases + 6


def manifests(lt):
    seen, over = 0, []
    for path in sorted(list((ROOT / 'apps').glob('*/manifest*.json')) + list((ROOT / 'checks/qemu-os').glob('*manifest*.json'))):
        try:
            text = json.loads(path.read_text())
        except ValueError:
            continue                                             # templates with build-time substitutions
        if 'deadlines' not in text:
            continue
        seconds = lt.derive(text)
        run_us, reserve_us = lt.declared(text)
        lt.check(run_us, reserve_us, seconds)
        assert 1 <= seconds <= lt.MAX_SECONDS, path
        seen += 1
        if run_us + reserve_us > 60_000_000:
            over.append((path.relative_to(ROOT).as_posix(), seconds))
    assert seen >= 15, f'only {seen} manifests found'
    assert any(name == 'apps/large-region-test/manifest-return.json' and seconds == 130 for name, seconds in over), over
    return seen, over


def limits(lt):
    header = (ROOT / 'include/a733_board_application.h').read_text()
    assert f'A733_BOARD_SUPERVISED_SECONDS > {lt.MAX_SECONDS}' in header, 'the header limit differs from MAX_SECONDS'
    host = load('lifetime_host', ROOT / 'apps/os/host.py')
    assert host.session_lifetime.MAX_SECONDS == lt.MAX_SECONDS
    images = (ROOT / 'tools/board_images.py').read_text()
    assert "<= SESSION.MAX_SECONDS" in images and "<= 600" not in images, 'the runner keeps its own marker bound'
    report = load('lifetime_thermal', ROOT / 'apps/os/thermal-report.py')
    assert report.MAX_ROWS == lt.thermal_rows(lt.MAX_SECONDS) and report.ROW_BYTES == lt.THERMAL_ROW_BYTES
    c = (ROOT / 'apps/os/thermal.c').read_text()
    assert '#define THERMAL_RECORDS (A733_BOARD_SUPERVISED_SECONDS*10u+2u)' in c and lt.THERMAL_ROWS_PER_SECOND == 10 and lt.THERMAL_EXTRA_ROWS == 2
    assert lt.CRUMB_WRAP_SECONDS > lt.MAX_SECONDS


def table():
    crumbs = load('lifetime_crumbs', ROOT / 'tools/a733-crumbs.py')
    terms, subs = crumbs.refusal_terms()
    header = (ROOT / 'include/a733_admission_refusal.h').read_text()
    assert len(terms) == len(re.findall(r'A733_REFUSAL_[A-Z_]+ = \d+,', header)) and terms[6] == 'session_decode'
    for number, group in subs.items():
        assert all(0 < sub <= 15 for sub in group), (number, group)
    assert all(0 < number <= 15 for number in terms)
    named = crumbs.refusal_detail(0x168)
    assert named['term'] == 'session_decode' and named['sub'] == 'run_deadline', named
    assert crumbs.refusal_detail(0x1) is None and crumbs.refusal_detail(0x2) is None and crumbs.refusal_detail(0x200) is None
    for number, group in subs.items():
        for sub, name in group.items():
            detail = 0x100 | number << 4 | sub
            assert 0x100 <= detail <= 0x1ff and crumbs.refusal_detail(detail)['sub'] == name
    # An EL3 stop word as the board records it: status 1 in bits 12..15, the detail below.
    word = dict(tag=0x5c0d0001, last=crumbs.EL3_STOP << 16 | 1 << 12 | 0x168, time=0, fault=0, esr=0, elr=0, far=0)
    decoded = crumbs.decode(word, 1)
    assert decoded['last']['admission_refusal']['sub'] == 'run_deadline', decoded['last']
    assert 'session_decode (run_deadline)' in decoded['summary'], decoded['summary']
    legacy = crumbs.decode({**word, 'last': crumbs.EL3_STOP << 16 | 1 << 12 | 1}, 1)
    assert 'unnamed admission refusal' in legacy['last']['admission_refusal']['meaning']
    return len(terms), sum(len(g) for g in subs.values())


def sources():
    """The consumers of the lifetime macro compile it, never a figure."""
    users = ['apps/os/session.c', 'apps/os/thermal.c', 'apps/os/cpu-return.c', 'platform/qemu-os/board.c', 'apps/board/radio_profile.c',
             'apps/os/prepare.py', 'apps/board/admission.c']
    for name in users:
        text = (ROOT / name).read_text()
        assert 'A733_BOARD_SUPERVISED_SECONDS' in text, name
        assert not re.search(r'(?<![\w.])(?:lifetime|session)[\w_]*\s*=\s*60\b', text), f'{name} compiles a lifetime figure'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    lt = load('lifetime_rule', ROOT / 'tools/session_lifetime.py')
    cases = rule(lt)
    seen, over = manifests(lt)
    limits(lt)
    terms, subs = table()
    sources()
    (args.output / 'result.json').write_text(json.dumps({'status': 'PASS', 'rule_cases': cases, 'manifests': seen, 'over_sixty': over,
                                                          'terms': terms, 'sub_terms': subs}, indent=2) + '\n')
    print(f'PASS session lifetime: {cases} rule cases; {seen} manifests derive a lifetime the decoder admits '
          f'({len(over)} needed more than the earlier fixed 60 s); {terms} refusal terms with {subs} sub-terms agree with the decoder table')


if __name__ == '__main__':
    main()
