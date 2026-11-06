#!/usr/bin/env python3
"""Check the Wi-Fi credential path with a fake credential and no board.

A fake network lives in a temporary config directory under the output directory, never in ~/.config. The check shows:

- the PMK derivation matches the IEEE 802.11i test vectors, and the fingerprint is HMAC-SHA256 of the PMK under the label;
- `add` runs under a pseudo-terminal, never echoes the password, and writes the directory at 0700 and the file at 0600;
- `list` prints names and SSIDs only;
- a submission records the name and the fingerprint, a dry-run launch delivers the record to a modeled board's memory by
  readback, and no byte of the password or the PMK, raw, hex or base64, appears in any file of the queue, the entry, the
  ledger, state.json or the result;
- the scan fails when the PMK or the password is written into the entry (mutation);
- a missing file, a changed fingerprint, a bad file mode and a bad directory mode refuse the run before launch, and so
  does an image whose application does not declare the radio device;
- the OS receiver (apps/os/wifi_credential.c) decodes the record, refuses malformed ones and wipes it.

No device is opened.
"""
import argparse
import base64
import contextlib
import hashlib
import hmac
import io
import json
import os
from pathlib import Path
import pty
import re
import struct
import shutil
import subprocess
import sys
import types
from unittest.mock import patch

if not __debug__:
    raise RuntimeError('Checks require assertions.')
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import board
import board_images as images

GATE_RUNS = [['--output', '{output}']]  # the live queue directory it reads is only compared before and after, never written
CONFIG_ENVIRONMENT = 'SUNSTONEOS_WIFI_CONFIG_DIR'
REAL_CONFIG = Path(os.path.expanduser('~'))/'.config/sunstoneos/wifi'
SSID, PASSWORD = 'SunstoneCheckNet', 'fake-pass-1103-xyz'
rows = []


def snapshot_real_residue():
    """The live queue's credential-residue directory: names, times and digests. A check must leave it as it found it."""
    live = Path('~/.cache/sunstoneos/board-queue/credential-residue').expanduser()
    return {p.name: (p.stat().st_mtime_ns, hashlib.sha256(p.read_bytes()).hexdigest())
            for p in sorted(live.glob('*'))} if live.is_dir() else {}


def passed(name):
    rows.append(name)
    print('PASS', name)


def trash(path, label):
    """Never delete: move the file aside, inside the check's own output directory."""
    aside = Path(os.environ[CONFIG_ENVIRONMENT]).parent/'moved-aside'
    aside.mkdir(exist_ok=True)
    shutil.move(str(path), str(aside/('%s-%s' % (label, os.getpid()))))


def forms(wifi, entry):
    return wifi.secrets_of(entry)


def scan(root, needles):
    """Every file under root, byte for byte: the needles must be absent. Returns the number of files read."""
    count = 0
    for path in sorted(Path(root).rglob('*')):
        if path.is_file() and not path.is_symlink():
            data = path.read_bytes()
            count += 1
            for needle in needles:
                if needle in data:
                    raise AssertionError('secret bytes found in %s' % path)
    return count


def vector_cases(wifi):
    assert wifi.selftest() == 3
    for password, ssid, expected in wifi.VECTORS:
        assert wifi.pmk(password, ssid).hex() == expected
        # An independent PBKDF2-HMAC-SHA1 (RFC 8018) for the first block pair.
        mac = lambda key, data: hmac.new(key, data, hashlib.sha1).digest()
        out = b''
        for block in (1, 2):
            u = mac(password.encode(), ssid.encode()+block.to_bytes(4, 'big'))
            t = int.from_bytes(u, 'big')
            for _ in range(4095):
                u = mac(password.encode(), u)
                t ^= int.from_bytes(u, 'big')
            out += t.to_bytes(20, 'big')
        assert out[:32].hex() == expected
    key = wifi.pmk('password', 'IEEE')
    assert wifi.fingerprint(key) == hmac.new(b'sunstoneos wifi credential fingerprint v1', key, hashlib.sha256).digest()[:8].hex()
    assert len(wifi.fingerprint(key)) == 16
    passed('pmk-matches-ieee-802.11i-vectors-and-fingerprint-is-truncated-hmac')


def tool_cases(wifi, work):
    config = work/'config'
    tool = ROOT/'tools/wifi-credential.py'
    # add under a pseudo-terminal: the password is typed twice and is never echoed.
    environment = dict(os.environ, **{wifi.CONFIG_ENVIRONMENT: str(config)})
    answers = [SSID, PASSWORD, PASSWORD, 'y']
    child, descriptor = pty.fork()
    if child == 0:
        os.execve(sys.executable, [sys.executable, '-I', str(tool), 'add', 'home'], environment)
    transcript = b''
    typed = 0
    while True:
        try:
            chunk = os.read(descriptor, 4096)
        except OSError:
            break
        if not chunk:
            break
        transcript += chunk
        prompts = (b'SSID: ', b'Password (not shown): ', b'Password again: ', b'[y/N] ')
        while typed < len(answers) and transcript.rstrip(b'\0').endswith(prompts[typed]):
            os.write(descriptor, answers[typed].encode()+b'\n')
            typed += 1
            transcript += b'<typed>'
    _, status = os.waitpid(child, 0)
    assert os.waitstatus_to_exitcode(status) == 0 and typed == 4, transcript
    assert PASSWORD.encode() not in transcript, 'password echoed'
    entry = wifi.load('home', config)
    assert os.stat(config).st_mode & 0o777 == 0o700 and os.stat(config/'home.json').st_mode & 0o777 == 0o600
    assert entry['ssid'] == SSID and entry['pmk'] == wifi.pmk(PASSWORD, SSID) and entry['password'] == PASSWORD
    assert not [p for p in config.iterdir() if p.name != 'home.json'], 'a temporary file remained'
    assert wifi.fingerprint(entry['pmk']).encode() in transcript
    passed('add-prompts-never-echoes-and-writes-0700-0600-atomically')
    # Without a terminal the tool refuses and writes nothing.
    result = subprocess.run([sys.executable, '-I', str(tool), '--config-dir', str(work/'refused'), 'add', 'x'],
                            capture_output=True, text=True, stdin=subprocess.DEVNULL)
    assert result.returncode == 2 and 'own terminal' in result.stderr and not (work/'refused').exists()
    # The no-password variant stores no password.
    wifi.store('nopass', SSID, PASSWORD, False, config)
    assert 'password' not in json.loads((config/'nopass.json').read_text())
    listing = subprocess.run([sys.executable, '-I', str(tool), '--config-dir', str(config), 'list'], capture_output=True, text=True)
    assert listing.returncode == 0 and 'home' in listing.stdout and SSID in listing.stdout
    for needle in forms(wifi, entry):
        assert needle not in listing.stdout.encode()
    printed = subprocess.run([sys.executable, '-I', str(tool), '--config-dir', str(config), 'fingerprint', 'home'],
                             capture_output=True, text=True)
    assert printed.stdout.strip() == wifi.fingerprint(entry['pmk'])
    for bad in ('../x', '.x', 'a/b', '', 'x'*41):
        try:
            wifi.check_name(bad)
        except wifi.CredentialError:
            pass
        else:
            raise AssertionError('name accepted: %r' % bad)
    wifi.remove('nopass', config)
    assert not (config/'nopass.json').exists()
    passed('list-and-fingerprint-print-no-secret-and-names-are-validated')
    return config


def arguments(root, *extra):
    parser = argparse.ArgumentParser()
    board.run_options(parser, True)
    parser.add_argument('--evidence', type=Path)
    parser.add_argument('--worker', default='worker')
    parser.add_argument('--hypothesis', default='the credential reaches the board without entering the queue')
    image = root/'source.bin'
    image.write_bytes(b'MODEL ONLY - never launch')
    return parser.parse_args([str(image), '--port', '3-1', '--evidence', str(root/'run'), '--kind', 'probe',
                              '--reference', 'docs/board-runner.md#submit-a-frozen-image-and-hypothesis', *extra])


def run_arguments(command):
    parser = argparse.ArgumentParser()
    board.run_options(parser)
    parser.add_argument('--evidence', type=Path)
    return parser.parse_args(command[4:])


class Board:
    """A modeled FEL and board access: write-then-read transfers into sparse memory, nothing else."""
    def __init__(self):
        self.memory = {}
        self.node = Path('/MODEL/003/001')

    def identity(self, node): return [1, 1, 3, 4, 5]
    def target(self, node): return Path('/MODEL/3-1')
    def require_rom(self, node): pass

    def process(self, command, **keywords):
        arguments = command[3:]
        assert arguments[0] == 'write' and arguments[3] == 'read' and arguments[1] == arguments[4]
        data = Path(arguments[2]).read_bytes()
        self.memory[int(arguments[1], 0)] = data
        assert int(arguments[5]) == len(data)
        Path(arguments[6]).write_bytes(self.memory[int(arguments[4], 0)])
        return types.SimpleNamespace(returncode=0, stdout='', stderr='')


class Dry(board.Live):
    """Live with its device stages replaced; launch performs the credential delivery the real launch performs."""
    model = None

    def __init__(self, args, evidence, record):
        super().__init__(args, evidence, record, access=Dry.model, process=Dry.model.process)

    @contextlib.contextmanager
    def locked(self):
        yield self

    def sanity(self): self.node, self.identity = Dry.model.node, Dry.model.identity(None)
    def usb_source_off(self, *a, **k): pass
    def reset(self): pass
    def dram(self): pass
    def prepare(self, info):
        return {'format': 'generic', 'uploads': [], 'credential': {'address': wifi_module().RECORD_ADDRESS,
                                                                   'bytes': wifi_module().RECORD_BYTES}}

    def launch(self, plan):
        self.deliver_credential(plan)
        self.record['launch_attempted'] = True
        return 'returned'

    def capture(self, plan): return {'payload': b'', 'report': {}, 'valid': True}


def wifi_module():
    return board.wifi_module()


def dry_run(queue, job, wifi, entry, radio=True):
    """Claim the job and run it as the daemon's child would, with the modeled board. radio: the image's application
    declares the radio device."""
    Dry.model = Board()
    with patch.object(board, 'port_idle', return_value=True):
        claimed = queue.claim('3-1')
    if claimed is None:
        return queue.job(job['id'])['result'], None
    command = queue.runner_command(claimed, '3-1')
    assert '--wifi' in command and '--wifi-fingerprint' in command
    assert not [c for c in command if any(n in c.encode() for n in forms(wifi, entry))], 'secret on the runner command line'
    run = run_arguments(command)
    info = {'generic': True, 'entry_guard': False, 'finite_npu': False}
    stub = types.SimpleNamespace(verify_delivered=lambda a: None)
    real_load = images.load
    host = real_load('wifi_check_records', ROOT/'apps/os/host.py')
    declared = host.manifest_tool.DEVICES['radio'] if radio else 0
    records = types.SimpleNamespace(manifest_tool=host.manifest_tool,
                                    image_manifest=lambda image: {'manifest': {'devices_required': 0, 'devices_optional': declared}})
    stubs = {'storage_bounds': stub, 'wifi_records': records}
    with patch.object(images, 'inspect', return_value=info), patch.object(images, 'policy'), \
         patch.object(images, 'dma_admission', return_value=None), patch.object(board, 'application_inputs'), \
         patch.object(images, 'symbols', return_value={'ss_wifi_credential': (0x41000000, 4)}), \
         patch.object(images, 'load', side_effect=lambda n, p: stubs[n] if n in stubs else real_load(n, p)), \
         contextlib.redirect_stdout(io.StringIO()):
        result = board.run_image(run, backend=lambda a, e, r: Dry(a, e, r))
    queue.complete(job['id'], result)
    return result, Dry.model


def queue_cases(wifi, config, work):
    entry = wifi.load('home', config)
    os.environ[wifi.CONFIG_ENVIRONMENT] = str(config)
    assert Path(wifi.config_directory()) != REAL_CONFIG
    root = work/'queue-case'
    root.mkdir()
    queue = board.Queue(root/'queue')
    args = arguments(root, '--wifi', 'home')
    job = queue.submit(args)
    fingerprint = wifi.fingerprint(entry['pmk'])
    assert job['wifi'] == {'name': 'home', 'fingerprint': fingerprint}
    assert job['options']['wifi'] == 'home' and job['options']['wifi_fingerprint'] == fingerprint
    entry_record = json.loads((Path(job['directory'])/'entry.json').read_text())
    assert entry_record['wifi'] == {'name': 'home', 'fingerprint': fingerprint}
    passed('submit-records-only-name-and-fingerprint')
    result, model = dry_run(queue, job, wifi, entry)
    assert result['outcome'] == 'returned' and result['wifi'] == {'name': 'home', 'fingerprint': fingerprint}, result
    record = wifi.board_record(entry)
    assert model.memory[wifi.RECORD_ADDRESS] == record and len(record) == 256
    done = queue.job(job['id'])
    assert done['result']['wifi'] == {'name': 'home', 'fingerprint': fingerprint}
    assert json.loads((Path(job['directory'])/'run/result.json').read_text())['wifi']['name'] == 'home'
    assert json.loads((Path(job['directory'])/'run/result.json').read_text())['wifi_delivered'] is True
    transfers = [t['arguments'] for t in json.loads((Path(job['directory'])/'run/session.json').read_text())['transactions']]
    assert any(a[0] == 'write' and a[1] == hex(wifi.RECORD_ADDRESS) and a[2].startswith('/proc/self/fd/') for a in transfers)
    passed('dry-run-launch-delivers-the-record-by-readback-from-memory')
    needles = forms(wifi, entry)
    # Positive control: the scanner sees the secret where it really is, in the modeled board memory and the record.
    assert any(n in model.memory[wifi.RECORD_ADDRESS] for n in needles)
    count = scan(queue.root, needles)
    assert count >= 8, count
    kinds = {p.name for p in queue.root.rglob('*') if p.is_file()}
    assert {'state.json', 'ledger.jsonl', 'entry.json', 'result.json', 'session.json'} <= kinds, kinds
    passed('no-secret-byte-in-any-queue-entry-ledger-state-or-result-file (%d files scanned)' % count)
    # Mutation: writing the PMK, or the password, into the entry, ledger or state must fail the scan.
    for label, payload, place in (('pmk-raw', entry['pmk'], Path(job['directory'])/'note.bin'),
                                  ('pmk-hex', entry['pmk'].hex().encode(), Path(job['directory'])/'run/note.txt'),
                                  ('pmk-base64', base64.b64encode(entry['pmk']), queue.root/'ledger.jsonl'),
                                  ('password', PASSWORD.encode(), queue.root/'state.json')):
        original = place.read_bytes() if place.exists() else None
        place.write_bytes((original or b'')+payload)
        try:
            scan(queue.root, needles)
        except AssertionError:
            pass
        else:
            raise AssertionError('mutation %s not detected' % label)
        if original is None:
            trash(place, label)
        else:
            place.write_bytes(original)
    scan(queue.root, needles)
    passed('mutation-writing-the-pmk-or-password-into-the-queue-fails-the-scan')
    return entry


def refusal_cases(wifi, config, work):
    entry = wifi.load('home', config)
    needles = forms(wifi, entry)

    def submit(name):
        root = work/('refusal-'+name)
        root.mkdir()
        queue = board.Queue(root/'queue')
        return queue, queue.submit(arguments(root, '--wifi', 'home'))

    def refused(queue, job, text):
        result, model = dry_run(queue, job, wifi, entry)
        assert model is None and result['outcome'] == 'refused-before-launch' and not result['launch_attempted'], result
        assert text in result['first_failure']['detail'], result['first_failure']
        assert result['wifi']['name'] == 'home'
        scan(queue.root, needles)
        return result

    original = (config/'home.json').read_bytes()
    # A missing file.
    queue, job = submit('missing')
    trash(config/'home.json', 'missing')
    refused(queue, job, 'does not exist')
    try:
        board.Queue(work/'refusal-missing-submit').submit(arguments(work/'refusal-missing', '--wifi', 'home'))
    except ValueError as error:
        assert 'does not exist' in str(error)
        assert not (work/'refusal-missing-submit/entries').exists() or not list((work/'refusal-missing-submit/entries').iterdir())
    else:
        raise AssertionError('submit accepted a missing credential')
    # The direct runner path refuses before it builds a backend.
    queue_args = arguments(work/'refusal-missing', '--wifi', 'home')
    queue_args.wifi_fingerprint = job['wifi']['fingerprint']
    queue_args.image = work/'refusal-missing/source.bin'
    direct = board.run_image(queue_args, backend=lambda *a: (_ for _ in ()).throw(AssertionError('backend built')))
    assert direct['outcome'] == 'refused-before-launch' and not direct['launch_attempted']
    passed('missing-file-refused-at-submit-claim-and-run-before-launch')
    (config/'home.json').write_bytes(original)
    os.chmod(config/'home.json', 0o600)
    # A changed fingerprint: the same name now holds another password.
    queue, job = submit('changed')
    wifi.store('home', SSID, PASSWORD+'-changed', True, config)
    result = refused(queue, job, 'changed since submit')
    assert result['wifi']['fingerprint'] == job['wifi']['fingerprint']
    (config/'home.json').write_bytes(original)
    os.chmod(config/'home.json', 0o600)
    passed('changed-fingerprint-refused-before-launch')
    # A bad file mode and a bad directory mode.
    queue, job = submit('file-mode')
    os.chmod(config/'home.json', 0o644)
    refused(queue, job, 'mode 0644')
    os.chmod(config/'home.json', 0o600)
    queue, job = submit('directory-mode')
    os.chmod(config, 0o755)
    refused(queue, job, 'mode 0755')
    os.chmod(config, 0o700)
    queue, job = submit('link')
    (config/'home.json').rename(config/'moved')
    (config/'home.json').symlink_to(config/'moved')
    refused(queue, job, 'not a plain file')
    (config/'home.json').unlink()
    (config/'moved').rename(config/'home.json')
    passed('bad-file-mode-bad-directory-mode-and-link-refused-before-launch')
    # Every OS image links the receiver; an application that does not declare the radio device never reads the credential.
    queue, job = submit('no-radio')
    result, model = dry_run(queue, job, wifi, entry, radio=False)
    assert result['outcome'] == 'refused-before-launch' and not result['launch_attempted'], result
    assert 'does not declare the radio device' in result['first_failure']['detail'], result['first_failure']
    assert model is None or not model.memory, 'the board was written before the refusal'
    scan(queue.root, needles)
    passed('application-without-the-radio-device-refused-before-launch')
    session_rule_cases(images.load('wifi_rule_records', ROOT/'apps/os/host.py'))
    wifi.load('home', config)


def session_rule_cases(host):
    """The session that receives a credential is one that wipes it: tools/board_images.py credential_session, which
    prepare calls whenever a credential is named, refuses any other session before an upload."""
    import inspect
    radio = host.manifest_tool.DEVICES['radio']
    admitted = {'admitted': True}

    def session(workload, devices):
        raw = bytearray(592 + 64)
        struct.pack_into('<I', raw, 400, workload)
        struct.pack_into('<I', raw, 592 + 24, devices)
        return bytes(raw)

    def refusal(raw, admission):
        try:
            images.credential_session(raw, admission, radio)
        except ValueError as error:
            return str(error)
        return None
    for workload in (1, 3):
        assert refusal(session(workload, radio | 1), admitted) is None, workload
    for workload, devices, admission, text in (
            (1, 1, admitted, 'grants the radio device'),          # the radio workload without the radio grant
            (3, 0, admitted, 'grants the radio device'),
            (0, radio | 1, admitted, 'radio workload'),           # the radio grant with a workload whose failure does not wipe
            (2, radio, admitted, 'radio workload'),
            (4, radio | 1, admitted, 'radio workload'),
            (1, radio | 1, {'admitted': None, 'unavailable': 'no harness'}, 'host admission is unavailable'),
            (1, radio | 1, {}, 'host admission is unavailable')):
        detail = refusal(session(workload, devices), admission)
        assert detail and text in detail, (workload, devices, admission, detail)
    # prepare names the rule wherever a credential is named, after the host admission of the session it uploads.
    source = inspect.getsource(images.prepare)
    branch = source[source.index("if getattr(args, 'wifi', None):"):]
    assert branch.index('credential_session(session, host_admission,') < branch.index("plan['credential'] ="), branch[:400]
    assert source.index('host_admission = preflight_admission(') < source.index('credential_session(')
    passed('credential-refused-unless-the-session-has-the-radio-grant-a-radio-workload-and-host-admission')


def board_side_cases(wifi, work):
    header = (ROOT/'include/sunstone_wifi_credential.h').read_text()
    assert int(re.search(r'SS_WIFI_CREDENTIAL_ADDRESS UINT64_C\((0x[0-9a-f]+)\)', header).group(1), 16) == wifi.RECORD_ADDRESS
    assert int(re.search(r'RECORD_BYTES (\d+)u', header).group(1)) == wifi.RECORD_BYTES
    span = (0x43000000, 0x43100000)
    assert span[0] <= wifi.RECORD_ADDRESS and wifi.RECORD_ADDRESS + wifi.RECORD_BYTES <= span[1]
    assert wifi.RECORD_ADDRESS >= 0x43000000+1872 and wifi.RECORD_ADDRESS+wifi.RECORD_BYTES <= 0x43008000, 'overlaps a profile'
    entry = {'ssid': SSID, 'pmk': wifi.pmk(PASSWORD, SSID), 'password': PASSWORD}
    record = wifi.board_record(entry)
    (work/'record.bin').write_bytes(record)
    (work/'ssid.bin').write_bytes(SSID.encode())
    (work/'pmk.bin').write_bytes(entry['pmk'])
    (work/'password.bin').write_bytes(PASSWORD.encode())
    for flag in ('-O0', '-O2'):
        binary = work/('receiver'+flag)
        subprocess.run(['clang', flag, '-Wall', '-Wextra', '-Werror', '-I', str(ROOT/'include'), str(ROOT/'checks/wifi-credential.c'),
                        '-o', str(binary)], check=True)
        run = subprocess.run([str(binary), str(work/'record.bin'), str(work/'ssid.bin'), str(work/'pmk.bin'), str(work/'password.bin')], capture_output=True, text=True)
        assert run.returncode == 0 and run.stdout.strip() == 'PASS', run.stdout
        for needle in forms(wifi, entry):
            assert needle not in (run.stdout+run.stderr).encode()
    # The wipe and the copy clear are real stores at -O2, not removed as dead.
    listing = subprocess.run(['objdump', '-d', '--no-show-raw-insn', str(work/'receiver-O2')], capture_output=True, text=True).stdout
    for function in ('os_wifi_credential_wipe',):
        body = re.search(r'<%s>:\n(.*?)\n\n' % function, listing, re.S)
        assert body and re.search(r'\bmovb\s+\$0x0|\bmov\s+byte ptr', body.group(1)), function
    # The target build: the same source for AArch64, with the cache clean.
    out = work/'receiver-a64.o'
    subprocess.run(['clang', '--target=aarch64-none-elf', '-ffreestanding', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT/'include'), '-I', str(ROOT/'runtime/include'), '-c', str(ROOT/'apps/os/wifi_credential.c'),
                    '-o', str(out)], check=True)
    disassembly = subprocess.run(['/usr/bin/llvm-objdump-21', '-d', str(out)], capture_output=True, text=True).stdout
    assert 'dc\tcivac' in disassembly and 'strb' in disassembly
    # The value reaches no result word, failure note or diagnostic: the receiver has no formatting or recording call.
    source = re.sub(r'/\*.*?\*/', '', (ROOT/'apps/os/wifi_credential.c').read_text(), flags=re.S)
    assert not re.search(r'printf|snprintf|crumb|board_fail|log\(|result', source)
    # Both session-end hooks name the wipe, and the cmake radio build links the receiver.
    assert 'os_wifi_credential_wipe();' in (ROOT/'apps/os/application.c').read_text()
    assert 'os_wifi_credential_wipe();' in (ROOT/'apps/board/main.c').read_text()
    assert 'apps/os/wifi_credential.c' in (ROOT/'cmake/sunstone-os-core.cmake').read_text()
    passed('os-receiver-decodes-refuses-malformed-and-wipes (host -O0 -O2, AArch64 object)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='new directory for the fake config and queue')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    work = args.output.resolve()
    wifi = board.wifi_module()
    real = snapshot_real_residue()
    vector_cases(wifi)
    config = tool_cases(wifi, work)
    queue_cases(wifi, config, work)
    refusal_cases(wifi, config, work)
    board_side_cases(wifi, work)
    assert snapshot_real_residue() == real, 'the check left a file in the live credential-residue directory'
    passed('no-file-left-in-the-live-credential-residue-directory')
    board.save(work/'results.json', rows)
    print('%d checks passed; fake credential only, no device access' % len(rows))


if __name__ == '__main__':
    main()
