#!/usr/bin/env python3
"""Run returning A733 images and service the local per-board run queue."""
import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import struct
import subprocess
import sys
import time
import traceback
import uuid

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import board_images as images

ROOT, CACHE = images.ROOT, images.CACHE
PYTHON = CACHE / 'venv/bin/python3'
PORTS = ('3-1', '3-3')
OUTCOMES = ('returned', 'result-mismatch', 'watchdog-reset', 'no-return', 'refused-before-launch', 'host-error')
crumbs = images.load('board_crumbs', ROOT/'tools/a733-crumbs.py')
dev = images.load('board_dev', ROOT/'tools/dev-run.py')
require = images.require


def save(path, value):
    """Durable replacement; keep the previous bytes until replacement succeeds."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
    with temporary.open('x') as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)
    fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


class TransportFailure(RuntimeError):
    pass


class NoReturn(RuntimeError):
    pass


class DramFailure(RuntimeError):
    pass


class DdrAbort(BaseException):
    """Escape the legacy DDR fallback after a terminal transport failure."""


class Mismatch(ValueError):
    pass


class Live:
    """The only FEL owner. Every request verifies the selected current instance."""
    def __init__(self, args, evidence, record, access=None, process=None, now=None, sleep=None, elapsed_now=None):
        self.args, self.evidence, self.record = args, evidence, record
        self.access = access or images.load('board_access', ROOT/'tools/usb-access.py')
        self.process = process or subprocess.run
        self.now, self.sleep = now or time.monotonic, sleep or time.sleep
        self.elapsed_now = elapsed_now or (now if now is not None else lambda: time.clock_gettime(time.CLOCK_BOOTTIME))
        self.node = self.identity = None
        self.failed = False
        self.quarantined = False
        self.sequence = 0
        self.ddr_arguments = []
        self.armed = False
        self.deadline = None
        self.journal_error = None
        self.recovery_deadline = None
        self.recovering = False

    def journal(self):
        try:
            save(self.evidence/'session.json', self.record)
        except OSError as error:
            self.journal_error = error

    @contextlib.contextmanager
    def locked(self):
        # Explicit discovery prevents this process from opening another board.
        node = self.access.discover(self.args.port)
        with self.access.board_lock(node) as node:
            self.adopt(node)
            try:
                yield self
            except TransportFailure as error:
                if self.armed and not self.recovering:
                    # Never communicate with the failed instance. Keep its lock
                    # through the bounded watchdog window and observe metadata.
                    try:
                        self.recovering = True
                        self.wait(max(0, self.recovery_deadline-self.now()))
                        self.disarm()
                        self.record['transport_recovered'] = True
                    except (NoReturn, TransportFailure):
                        self.quarantine(str(error))
                        raise
                else:
                    self.quarantine(str(error))
                raise
            except NoReturn as error:
                self.quarantine(str(error))
                raise

    def adopt(self, node):
        self.access.require_session(node)
        self.access.require_rom(node)
        require(self.access.target(node).name == self.args.port, 'Physical target changed.')
        self.node, self.identity = node, self.access.identity(node)
        self.record.setdefault('instances', []).append({'node': str(node), 'identity': self.identity})
        self.journal()

    def quarantine(self, reason):
        if not self.quarantined and self.node is not None:
            self.access.quarantine_instance(self.node, reason, self.identity)
            self.quarantined = True
            self.record['quarantined'] = True
            self.journal()

    def tx(self, *arguments, timeout=30, pass_fds=()):
        require(not self.failed, 'A failed FEL transaction forbids further requests.')
        if self.journal_error and not self.recovering:
            raise self.journal_error
        arguments = list(map(str, arguments))
        entry = {'arguments': arguments, 'start': self.now()}
        self.record.setdefault('transactions', []).append(entry)
        self.journal()  # A file failure here precedes device communication.
        try:
            if self.access.identity(self.node) != self.identity or self.access.target(self.node).name != self.args.port:
                raise TransportFailure('USB identity changed before FEL request.')
            self.access.require_rom(self.node)
            result = self.process([str(self.args.fel), '-d', f'{int(self.node.parent.name)}:{int(self.node.name)}',
                                   *arguments], capture_output=True, text=True, timeout=timeout, pass_fds=pass_fds)
        except (OSError, subprocess.TimeoutExpired, TransportFailure) as error:
            self.failed = True
            raise TransportFailure(str(error)) from error
        # Record a failed transfer before any parsing or filesystem operation.
        if result.returncode:
            self.failed = True
            entry.update(exit_code=result.returncode, stderr=result.stderr, stdout=result.stdout)
            raise TransportFailure('FEL transaction failed: ' + result.stderr.strip())
        entry.update(exit_code=0, stdout=result.stdout, stderr=result.stderr, seconds=self.now()-entry['start'])
        try:
            self.journal()
        except OSError as error:
            # Hardware has already completed the request. Let the caller
            # classify/disarm it before reporting this host filesystem error.
            self.journal_error = error
        return result.stdout

    def read(self, address, size, path):
        self.tx('read', hex(address), size, path, timeout=max(30, size/100000+10))
        data = Path(path).read_bytes()
        require(len(data) == size, 'Incomplete FEL readback.')
        return data

    def upload(self, address, source):
        data = Path(source).read_bytes()
        self.sequence += 1
        # A sealed source prevents a concurrent build changing upload bytes.
        base = images.load('board_sealed', ROOT/'tools/check-a733-smp.py')
        path = self.evidence/f'readback-{self.sequence}.bin'
        with base.sealed_source(data) as (sealed, fd):
            self.tx('write', hex(address), sealed, 'read', hex(address), len(data), path,
                    timeout=max(30, len(data)*2/100000+10), pass_fds=(fd,))
        if path.read_bytes() != data:
            raise Mismatch(f'Upload readback differs at {address:#x}; execution refused.')

    def sanity(self):
        require('soc=00001903(A733)' in self.tx('version'), 'Target is not A733.')
        # readl uses SRAM scratch at 0x71000; preserve it before any ROM check.
        before = self.read(0x71000, 0x200, self.evidence/f'rom-scratch-{self.sequence}.bin')
        words = [int(x, 16) for x in self.tx(*[x for a in (0x20, 0x24, 0x28, 0x3c, 0x40)
                                             for x in ('readl', hex(a))]).split()]
        require(words == [0xeb00008f, 0xe3a00911, 0xe3a0193d, 0xe3a0605c, 0xea00004d], 'Forced-FEL ROM entry differs.')
        self.upload(0x71000, images.write(self.evidence/f'rom-scratch-restore-{self.sequence}.bin', before))

    def arm(self, mode, clear=True, configuration=1):
        self.recovery_deadline = self.now()+crumbs.interval(mode)+10
        args = crumbs.arm_arguments(mode)
        require(configuration in (1,0x101), 'Unreviewed watchdog clock configuration.')
        for index,value in enumerate(args):
            if value==hex(crumbs.WDT_CONFIG):
                args[index+1]=hex(crumbs.KEY|configuration)
        if not clear:
            args = args[len(crumbs.WORDS)*3:]
        self.armed = True
        self.tx(*args)
        values = [int(x,16)&0xffff for x in self.tx('readl', hex(crumbs.WDT_MODE), 'readl', hex(crumbs.WDT_CONFIG),
                                                  'readl', hex(crumbs.WDT_OUTPUT)).split()]
        require(values == [mode, configuration, 2], 'WDT0 arm readback differs.')

    def disarm(self):
        raw = self.tx('writel', hex(crumbs.WDT_MODE), hex(crumbs.KEY), 'readl', hex(crumbs.WDT_MODE))
        try:
            value = int(raw,16)
        except ValueError as error:
            self.failed = True
            raise TransportFailure('Invalid WDT0 disarm readback; state unknown.') from error
        self.confirm_disarmed(value)

    def confirm_disarmed(self, value):
        self.armed = bool(value&1)
        if self.armed:
            self.failed = True
            raise TransportFailure('WDT0 disarm readback stayed enabled; state unknown.')

    def returned(self):
        raw = self.tx(*crumbs.return_arguments())
        try:
            before,after,words = crumbs.parse_return(raw)
        except ValueError as error:
            # A host decoder failure must not leave a possibly armed image or
            # quarantine a healthy returned instance. Verify disarm separately.
            self.disarm()
            raise RuntimeError('Invalid watchdog return report: '+str(error)) from error
        self.confirm_disarmed(after)
        return before,after,words

    def wait(self, seconds, old=None):
        old = self.identity if old is None else old
        deadline = self.now() + seconds
        self.deadline = deadline
        while True:
            try:
                node = self.access.discover(self.args.port)
                current = self.access.identity(node)
                if current != old and os.access(node, os.R_OK | os.W_OK):
                    # Do not send a request before udev grants the new node.
                    self.adopt(node)
                    self.failed = False
                    self.deadline = None
                    return
            except (FileNotFoundError, PermissionError, ValueError):
                pass
            if self.now() >= deadline:
                break
            self.sleep(.05)
        raise NoReturn(f'No accessible new FEL instance within {seconds:g} seconds.')

    def reset(self):
        old = self.identity
        self.arm(0x41, clear=False)
        self.wait(14, old)
        self.disarm()
        self.sanity()
        self.record['fresh_fel'] = True

    def finish_pending(self):
        """A host error cannot release an armed run before its reset deadline."""
        if self.armed and not self.failed:
            self.recovering = True
            self.wait(max(0, self.recovery_deadline-self.now()))
            self.disarm()

    def dram(self):
        owner = self
        class Access:
            def __getattr__(self, name):
                return getattr(owner.access, name)
            @contextlib.contextmanager
            def board_lock(self):
                yield owner.node
        class Base:
            def __getattr__(self, name):
                return getattr(images.load('board_ddr_base', ROOT/'tools/check-a733-smp.py'), name)
            @staticmethod
            def stop_after_failure(record, save_record, quarantine, error, **kwargs):
                record['failure'] = str(error)
                save_record()
            @staticmethod
            def hold_ownership():
                pass
        class Guard:
            @staticmethod
            def check_health():
                pass
            @staticmethod
            @contextlib.contextmanager
            def reenumeration(*args):
                yield {'count': 1}
        self.ddr_access, self.ddr_base, self.ddr_guard = Access(), Base(), Guard()
        sid = [int(x,16) for x in self.tx(*[v for n in range(4) for v in ('readl', hex(0x03006200+4*n))]).split()]
        require(len(sid) == 4, 'SID readback differs.')
        self.record['sid'] = sid
        training = images.load('board_training', ROOT/'tools/training-record.py')
        selected = None
        rejected = []
        # The directory contains only coordinator-installed per-SID records.
        for path in sorted(self.args.training_dir.glob('*.bin')):
            try:
                parsed = training.parse(path.read_bytes())
            except (ValueError,struct.error,OSError) as error:
                rejected.append({'path':str(path),'reason':str(error)})
                continue
            if parsed['chip_id'] == sid:
                selected = path
                break
        self.record['dram_record_rejections'] = rejected
        self.ddr_arguments = ['--fel', str(self.args.fel), '--build', str(self.args.ddr_restore if selected else self.args.ddr_build),
                              '--evidence', str(self.evidence/'dram')]
        if selected:
            self.ddr_arguments += ['--record', str(selected), '--fallback-build', str(self.args.ddr_build)]
        self.record['dram_training_record'] = str(selected) if selected else None
        self.journal()
        try:
            # run-ddr keeps its checked restore/fallback sequence. Injection
            # replaces only ownership, transactions and terminal failure policy.
            with (self.evidence/'dram.log').open('w') as log, contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
                try:
                    runpy.run_path(str(ROOT/'tools/run-ddr.py'), init_globals={'BOARD_RUNNER': self})
                except SystemExit as status:
                    if status.code not in (0, 3, None):
                        raise DramFailure(str(status)) from status
            outcome = json.loads((self.evidence/'dram/session.json').read_text())
            require(outcome.get('complete'), 'DRAM bring-up did not complete.')
            # Restore fallback may have changed the instance inside run-ddr.
            self.adopt(self.access.discover(self.args.port))
        except (TransportFailure, NoReturn):
            raise
        except DdrAbort as error:
            raise TransportFailure(str(error)) from error
        except (ValueError, RuntimeError) as error:
            if self.failed:
                raise TransportFailure(str(error)) from error
            raise DramFailure(str(error)) from error

    def ddr_transaction(self, node, identity, record, save_record, commands, timeout):
        # run-ddr may have awaited a WDT reset through metadata only.
        if node != self.node or identity != self.identity:
            require(self.access.identity(node) == identity, 'DDR returned identity changed.')
            self.adopt(node)
            self.failed = False
        record['device_access_attempted'] = True
        record['launch_attempted'] |= 'exe' in commands
        # The full-training and cache-off helpers predate host WDT support.
        # Supervise every native helper while retaining the restore sequence.
        helper = 'exe' in commands and int(str(commands[list(commands).index('exe')+1]),0) in (0x71000, 0x85000)
        restore_armed = self.armed
        if helper and not restore_armed:
            self.arm(0xb1, clear=False)
        # The DDR script arms/disarms WDT with combined transactions.
        disarmed = False
        for i, op in enumerate(commands):
            if op == 'writel' and int(str(commands[i+1]),0) == crumbs.WDT_MODE:
                if int(str(commands[i+2]),0)&1:
                    self.armed = True
                    self.recovery_deadline = self.now()+crumbs.interval(int(str(commands[i+2]),0))+10
                else:
                    disarmed = True
        try:
            result = self.tx(*commands, timeout=timeout)
            if disarmed:
                try:
                    value = int(result.split()[-1],16)
                except (ValueError,IndexError) as error:
                    self.disarm()
                    raise RuntimeError('Invalid DDR watchdog disarm report.') from error
                self.confirm_disarmed(value)
            if helper and not restore_armed:
                self.disarm()
        except TransportFailure as error:
            record['transport_failed'] = True
            save_record()
            # A failed request to the old instance terminates that instance.
            # Do not let run-ddr's legacy fallback retry it.
            raise DdrAbort(str(error)) from error
        save_record()
        return result

    def rails(self):
        data = self.args.rail_helper.read_bytes()
        require(hashlib.sha256(data).hexdigest() == dev.RAIL_HELPER_SHA256, 'Rail helper binary differs from the pinned one.')
        oscillator = int(self.tx('readl','0x07090160'),16)
        require((oscillator>>14)&3==2 and oscillator&0x10000002==0x10000002,
                'Rail helper requires the enabled raw 26 MHz oscillator.')
        self.upload(0x41000000, images.write(self.evidence/'rail-helper.bin',data))
        self.upload(0x42000000, images.write(self.evidence/'rail-blank.bin', bytes(16384)))
        # This exact legacy helper returns via RMR but does not disarm WDT0.
        # A733 manual 8.3.6.5-6: configuration bit 8 selects DCXO/750;
        # interval b counts 512k cycles (14.769 s at raw 26 MHz). Complete
        # return/disarm strictly before 14 s; never infer live DDR after that.
        # BOOTTIME includes host suspend, unlike monotonic().
        armed_at = self.elapsed_now()
        self.arm(0xb1,configuration=0x101)
        self.tx('reset64', '0x41000000')
        self.wait(26)
        before, after, words = self.returned()
        elapsed = self.elapsed_now()-armed_at
        self.record['rail_return'] = {'seconds':elapsed,'watchdog_configuration':0x101,
            'source_hz':26000000/750,'mode_before_disarm':before,'mode_after_disarm':after,'cutoff_seconds':14}
        if not (before&0xffff==0xb1 and not after&1 and 0<=elapsed<14):
            self.record.update(dram_lost=True,setup_watchdog_reset=True)
            raise ValueError('Rail helper possible watchdog reset; no DRAM result read; rails were not normalized.')
        data = self.read(0x42000000, 16384, self.evidence/'rail-result.bin')
        values = struct.unpack_from('<16Q', data)
        require(values[3] <= 16384-257, 'Invalid rail helper log length.')
        log = data[256:256+values[3]].decode(errors='replace')
        (self.evidence/'rail.log').write_text(log)
        # The helper performs all boot-clock, speed-bin and PMIC readbacks.
        self.record['rails'] = images.rail_result(values, log)
        self.journal()

    def live_state(self):
        values = [int(v,16) for v in self.tx(*[x for a in dev.WORDS for x in ('readl', hex(a))]).split()]
        require(len(values) == len(dev.WORDS), 'Live state word count differs.')
        return {'identity': self.identity, 'port': self.args.port, 'words': {hex(a):v for a,v in zip(dev.WORDS,values)}}

    def prepare(self, info):
        return images.prepare(self.args, info, self.live_state(), self.evidence/'bundle')

    def launch(self, plan):
        for address, source in plan['uploads']:
            self.upload(address, source)
        self.record['launch_attempted'] = True
        self.journal()
        self.arm(plan['watchdog']['mode'])
        self.recovery_deadline = self.now()+plan['watchdog']['session_seconds']+crumbs.interval(plan['watchdog']['mode'])+10
        self.tx(*plan['launch'])
        self.wait(plan['watchdog']['session_seconds'] + crumbs.interval(plan['watchdog']['mode']) + 10)
        before, after, words = self.returned()
        decoded = crumbs.decode(words, plan['epoch'])
        self.record['breadcrumbs'] = decoded
        save(self.evidence/'breadcrumbs.json', decoded)
        require(not after & 1, 'WDT0 did not disarm after return.')
        self.record['rom_returned'] = True
        if before & 1:
            self.record['dram_lost'] = True
            return 'watchdog-reset'
        require(decoded['session'] and decoded.get('last',{}).get('stage') == crumbs.RETURN_ROM and
                decoded['last']['value'] == 0, 'Return lacks the matching RETURN_ROM breadcrumb.')
        self.record['normal_return'] = True
        return 'returned'

    def capture(self, plan):
        try:
            return images.capture(self, plan, self.args)
        except ValueError as error:
            raise Mismatch(str(error)) from error


def expectation(value):
    if value is None:
        return None
    path = Path(value)
    if path.is_file():
        return {'mode': 'bytes', 'hex': path.read_bytes().hex()}
    return {'mode': 'text', 'text': value}


def port_idle(port):
    """Inspect only the host lock; never open a USB device."""
    with Path(f'/tmp/sunstoneos-a733-{port}.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except BlockingIOError:
            return False


def process_identity(pid):
    try:
        fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')',1)[1].split()
        return {'pid':pid,'start_ticks':fields[19],'state':fields[0]}
    except FileNotFoundError:
        return None


def process_alive(identity):
    current = process_identity(identity['pid'])
    return bool(current and current['start_ticks']==identity['start_ticks'] and current['state']!='Z')


def references(args):
    """Hash the host preparation code and cached helper inputs used by a job."""
    paths = set()
    trees = {ROOT}
    if args.host_tree or args.kind=='gpu-return':
        trees.add(args.host_tree or images.GPU_TREE)
    for tree in trees:
        for directory in ('tools','checks','apps/os','apps/board','apps/cpu-runtime-qualification'):
            paths.update((tree/directory).glob('*.py'))
        for directory in ('platform/a733-smp-supervised','platform/a733-smp-observe','platform/a733-smp-cached','include'):
            paths.update(p for p in (tree/directory).glob('*') if p.is_file())
        paths.add(tree/'drivers/clock/a733_cpu_clock.c')
        paths.add(tree/'apps/board/launch.c')
    paths.update((args.fel,args.rail_helper))
    for directory in (args.ddr_build,args.ddr_restore):
        manifest = directory/'manifest.json'
        paths.add(manifest)
        if manifest.exists():
            paths.update(directory/name for name in json.loads(manifest.read_text())['files'])
    paths.update(args.training_dir.glob('*.bin'))
    return {str(p.resolve()):images.sha(p) for p in paths if p.is_file()}


def verify_references(inventory):
    require(all(Path(path).is_file() and images.sha(path)==digest for path,digest in inventory.items()),
            'Queued host code or helper inputs changed; resubmit the frozen image.')


def run_image(args, backend=Live):
    started = time.monotonic()
    evidence = args.evidence or CACHE/'board-queue/runs'/f'{time.time_ns()}-{args.port}-{uuid.uuid4().hex[:8]}'
    evidence.mkdir(parents=True, exist_ok=False)
    result = {'schema': 'sunstone-board-result-v1', 'port': args.port, 'image': str(args.image),
              'kind': args.kind, 'outcome': 'host-error', 'first_failure': None, 'durations': {},
              'evidence_path': str(evidence.resolve()), 'result_path': str((evidence/'result.json').resolve()),
              'quarantined': False, 'launch_attempted': False, 'time': time.time(),
              'git_revision': subprocess.run(['git','-C',str(ROOT),'rev-parse','HEAD'], capture_output=True,text=True).stdout.strip()}
    owner = None
    stage = 'preflight'
    def timed(name, action):
        nonlocal stage
        stage = name
        begin = time.monotonic()
        try:
            value = action()
            if owner is not None and owner.journal_error is not None:
                raise owner.journal_error
            return value
        finally:
            result['durations'][name] = round(time.monotonic()-begin,6)
    try:
        data = args.image.read_bytes()
        result['image_sha256'] = hashlib.sha256(data).hexdigest()
        frozen = evidence/('image.elf' if data[:4] == b'\x7fELF' else 'image.bin')
        frozen.write_bytes(data)
        descriptor_path = args.descriptor or Path(str(args.image)+'.board.json')
        descriptor = json.loads(descriptor_path.read_text()) if descriptor_path.exists() else None
        if descriptor:
            save(evidence/'image.board.json', descriptor)
        args.image = frozen
        if args.reference_inventory:
            verify_references(json.loads(args.reference_inventory.read_text()))
        info = images.inspect(frozen, args.kind, descriptor,args.host_tree)
        images.policy(info, args)
        expected = json.loads(args.expect_spec.read_text()) if args.expect_spec else expectation(args.expect)
        if expected is not None:
            save(evidence/'expect.json', expected)
        owner = backend(args, evidence, result)
        with owner.locked():
            try:
                timed('identity', owner.sanity)
                timed('reset', owner.reset)
                timed('dram', owner.dram)
                if info['entry_guard']:
                    timed('rails', owner.rails)
                plan = timed('prepare', lambda: owner.prepare(info))
                outcome = timed('launch', lambda: owner.launch(plan))
                result['outcome'] = outcome
                if outcome == 'returned':
                    captured = timed('capture', lambda: owner.capture(plan))
                    payload = captured.pop('payload')
                    (evidence/'output.bin').write_bytes(payload)
                    result['return_report'] = captured['report']
                    result['output_sha256'] = hashlib.sha256(payload).hexdigest()
                    matched = captured['valid']
                    if expected:
                        matched &= payload == bytes.fromhex(expected['hex']) if expected['mode'] == 'bytes' else expected['text'] in payload.decode(errors='replace')
                    if not matched:
                        raise Mismatch('Returned result differs from expectation.')
                else:
                    decoded = result.get('breadcrumbs',{})
                    fault = decoded.get('fault',{})
                    last = decoded.get('last',{})
                    result['first_failure'] = {'site': fault.get('elr') or last.get('name','before-entry'),
                                                'detail': decoded.get('summary','Watchdog reset')}
                if owner.journal_error is not None:
                    raise owner.journal_error
            except BaseException as error:
                # Wait out any armed helper/image if a host exception interrupted
                # the launch path. Never send another request to a failed instance.
                if owner.armed and not owner.failed and not isinstance(error, NoReturn):
                    owner.finish_pending()
                raise
    except NoReturn as error:
        result['outcome'] = 'no-return'
        result['first_failure'] = {'site': stage, 'detail': str(error)}
    except Mismatch as error:
        result['outcome'] = 'result-mismatch'
        result['first_failure'] = {'site': stage, 'detail': str(error)}
    except (ValueError, DramFailure) as error:
        result['outcome'] = 'host-error' if result.get('launch_attempted') else 'refused-before-launch'
        result['first_failure'] = {'site': stage, 'detail': str(error)}
    except BaseException as error:
        result['outcome'] = 'host-error'
        result['first_failure'] = {'site': stage, 'detail': f'{type(error).__name__}: {error}'}
        (evidence/'host-error.txt').write_text(traceback.format_exc())
    result['stage'] = stage
    result['durations']['total'] = round(time.monotonic()-started,6)
    # Raw transaction evidence has its own record; result.json stays compact.
    summary = {k:v for k,v in result.items() if k != 'transactions'}
    save(evidence/'result.json', summary)
    print(f"{args.port} {args.kind} {summary.get('image_sha256','unreadable')[:12]} {summary['outcome']} {evidence/'result.json'}", flush=True)
    return summary


class Queue:
    """One durable state file and an append-only event ledger under a short lock."""
    def __init__(self, root):
        self.root = Path(root).resolve()
        self.root.mkdir(parents=True, exist_ok=True)

    @contextlib.contextmanager
    def locked(self):
        with (self.root/'queue.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            path = self.root/'state.json'
            state = json.loads(path.read_text()) if path.exists() else {'jobs': [], 'pauses': {}}
            yield state
            save(path, state)

    def event(self, event, **fields):
        with (self.root/'ledger.jsonl').open('a') as stream:
            stream.write(json.dumps({'event':event, 'time':time.time(), **fields}, sort_keys=True)+'\n')
            stream.flush()
            os.fsync(stream.fileno())

    def pause(self, state, port, reason):
        if port in state['pauses']:
            return
        value = {'port': port, 'reason': reason, 'time': time.time()}
        state['pauses'][port] = value
        save(self.root/f'paused-{port}.json', value)
        self.event('paused', **value)

    def refused(self, state, job, detail):
        result = {'schema': 'sunstone-board-result-v1', 'outcome': 'refused-before-launch',
                  'image_sha256': job['image_sha256'], 'port': job['port'], 'kind': job['kind'],
                  'first_failure': {'site': 'queue', 'detail': detail}, 'quarantined': False,
                  'launch_attempted': False, 'durations': {}, 'evidence_path': job['directory']}
        save(job['result_path'], result)
        job.update(state='done', result=result, completed=time.time())
        self.event('result', job=job['id'], result_path=job['result_path'], result=result)

    def submit(self, args):
        require(re.fullmatch(r'[A-Za-z0-9_.-]{1,80}', args.worker), 'Invalid worker name.')
        require(bool(args.hypothesis.strip()), 'A nonempty hypothesis is required.')
        identifier = f'{time.time_ns()}-{uuid.uuid4().hex[:8]}'
        directory = self.root/'entries'/identifier
        directory.mkdir(parents=True, exist_ok=False)
        source = args.image.resolve()
        data = source.read_bytes()
        image = directory/('image.elf' if data[:4] == b'\x7fELF' else 'image.bin')
        image.write_bytes(data)
        # Freeze all caller-controlled launch inputs before queue visibility.
        options = {}
        for name in ('kind','approved_by','storage_write','host_tree','fel','rail_helper','ddr_build','ddr_restore','training_dir'):
            value = getattr(args,name)
            options[name] = str(value.resolve()) if isinstance(value,Path) else value
        options['input'] = []
        for i,path in enumerate(args.input):
            target = directory/f'input-{i}.bin'
            target.write_bytes(path.read_bytes())
            options['input'].append(str(target))
        descriptor = args.descriptor or Path(str(source)+'.board.json')
        if descriptor.exists():
            target = directory/'image.board.json'
            target.write_bytes(descriptor.read_bytes())
            options['descriptor'] = str(target)
        template = args.template or (images.GPU_TEMPLATE if args.kind=='gpu-return' else images.CONTROL)
        if args.kind!='probe':
            if args.kind=='gpu-return':
                shutil.copytree(template, directory/'template')
            else:
                # Generic sessions need service facts and manifest inputs,
                # without the old application's assets or binaries.
                destination = directory/'template'
                destination.mkdir()
                if (template/'session.bin').is_file():
                    shutil.copy2(template/'session.bin',destination/'session.bin')
                    for path in template.glob('input-*.bin'):
                        shutil.copy2(path,destination/path.name)
                else:
                    (destination/'board').mkdir()
                    shutil.copy2(template/'board/session-template.bin',destination/'board/session-template.bin')
            options['template'] = str(directory/'template')
        exp = expectation(args.expect)
        if exp:
            target = directory/'expect.json'
            save(target,exp)
            options['expect_spec'] = str(target)
        inventory = directory/'references.json'
        save(inventory,references(args))
        options['reference_inventory'] = str(inventory)
        # Capture the complete queued input inventory, including templates.
        frozen = {str(p.relative_to(directory)): images.sha(p) for p in directory.rglob('*') if p.is_file()}
        job = {'id': identifier, 'submitted': time.time(), 'image': str(image),
               'image_sha256': hashlib.sha256(data).hexdigest(), 'port': args.port, 'kind': args.kind,
               'worker': args.worker, 'hypothesis': args.hypothesis.strip(), 'state': 'queued',
               'directory': str(directory), 'result_path': str(directory/'run/result.json'), 'options': options, 'frozen': frozen}
        with self.locked() as state:
            history = state['jobs']
            repeated = next((j for j in history if j['worker'] == job['worker'] and
                             j['image_sha256'] == job['image_sha256'] and j['hypothesis'] == job['hypothesis']), None)
            history.append(job)
            save(directory/'entry.json', job)
            self.event('entry', entry=job)
            if repeated:
                port = repeated.get('assigned_port') or (args.port if args.port != 'any' else None)
                ports = [port] if port else PORTS
                for port in ports:
                    self.pause(state, port, f"{args.worker} repeated image {job['image_sha256'][:12]} with the same hypothesis")
                self.refused(state, job, 'Repeated image and hypothesis; submit a new hypothesis.')
            elif sum(j['worker'] == args.worker and job['submitted']-j['submitted'] < 3600 for j in history) > 40:
                self.refused(state, job, 'Runaway backstop: 40 submissions per worker per hour.')
        return job

    def claim(self, port):
        with self.locked() as state:
            if port in state['pauses'] or any(j['state']=='running' and j.get('assigned_port')==port for j in state['jobs']):
                return None
            job = next((j for j in state['jobs'] if j['state']=='queued' and j['port'] in ('any',port)), None)
            if job is None:
                return None
            if not port_idle(port):
                return None
            directory = Path(job['directory'])
            if any(not (directory/name).is_file() or images.sha(directory/name) != digest for name,digest in job['frozen'].items()):
                self.refused(state, job, 'Queued input bytes changed; execution refused.')
                return None
            try:
                verify_references(json.loads((directory/'references.json').read_text()))
            except ValueError as error:
                self.refused(state,job,str(error))
                return None
            job.update(state='running', assigned_port=port, started=time.time())
            self.event('running', job=job['id'], port=port)
            return dict(job)

    def complete(self, identifier, result):
        with self.locked() as state:
            job = next(j for j in state['jobs'] if j['id']==identifier)
            if job['state']=='done':
                return
            save(job['result_path'], result)
            job.update(state='done', result=result, completed=time.time())
            self.event('result', job=job['id'], result_path=job['result_path'], result=result)
            port = job['assigned_port']
            if result['outcome']=='no-return' or result.get('quarantined'):
                self.pause(state,port,'No return within the watchdog recovery window; inspect recovery before resuming.')
            history = [j for j in state['jobs'] if j.get('assigned_port')==port and j['state']=='done']
            if len(history)>=2 and all(j['result'].get('stage')=='dram' and j['result']['outcome']!='returned' for j in history[-2:]):
                self.pause(state,port,'Two consecutive DRAM bring-ups failed.')
            failure = result.get('first_failure')
            if failure and result['outcome'] not in ('returned','refused-before-launch'):
                matches = [j for j in history if j['hypothesis']==job['hypothesis'] and
                           (j['result'].get('first_failure') or {}).get('site')==failure['site']]
                if len(matches)>=3:
                    self.pause(state,port,f"Third failure at {failure['site']} with no new hypothesis.")

    def resume(self, port):
        self.recover()
        with self.locked() as state:
            state['pauses'].pop(port,None)
            path = self.root/f'paused-{port}.json'
            if path.exists():
                archive = self.root/'resumed'
                archive.mkdir(exist_ok=True)
                path.rename(archive/f'{time.time_ns()}-{port}.json')
            self.event('resumed',port=port)

    def recover(self, exclude=()):
        """Collect an orphaned child's result; never replay its image."""
        exclude = set(exclude)
        with self.locked() as state:
            interrupted = [dict(j) for j in state['jobs'] if j['state']=='running' and j['id'] not in exclude]
        for job in interrupted:
            port = job['assigned_port']
            path = Path(job['directory'])/'run/result.json'
            lease = Path(job['directory'])/'runner-process.json'
            identity = json.loads(lease.read_text()) if lease.exists() else job.get('process')
            alive = identity is not None and process_alive(identity)
            if path.exists() and port_idle(port) and not alive:
                self.complete(job['id'],json.loads(path.read_text()))
            else:
                with self.locked() as state:
                    self.pause(state,port,'Daemon interrupted during a run; inspect the runner and port lock.')
                if port_idle(port) and identity is not None and not alive:
                    self.complete(job['id'],{'outcome':'host-error','image_sha256':job['image_sha256'],
                        'port':port,'durations':{},'evidence_path':job['directory'],
                        'first_failure':{'site':'daemon-interrupted','detail':'No child result; image is not replayed.'}})

    def status(self):
        with self.locked() as state:
            return {'queue':str(self.root), 'boards':{port:{'pause':state['pauses'].get(port),
                'running':[j for j in state['jobs'] if j['state']=='running' and j.get('assigned_port')==port],
                'queued':[j for j in state['jobs'] if j['state']=='queued' and j['port'] in ('any',port)],
                'results':[j for j in state['jobs'] if j['state']=='done' and
                           j.get('assigned_port', j['port'])==port][-20:]} for port in PORTS}}

    def serve(self, once=False):
        with (self.root/'daemon.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX|fcntl.LOCK_NB)
            self.recover()
            active = {}
            try:
                while True:
                    self.recover(job['id'] for _,job,_ in active.values())
                    for port in PORTS:
                        if port in active:
                            process, job, log = active[port]
                            if process.poll() is None:
                                continue
                            log.close()
                            path = Path(job['directory'])/'run/result.json'
                            if path.exists():
                                result = json.loads(path.read_text())
                            else:
                                result = {'outcome':'host-error', 'first_failure':{'site':'runner-process','detail':f'exit {process.returncode}'},
                                          'image_sha256':job['image_sha256'], 'port':port, 'durations':{}, 'evidence_path':job['directory']}
                            self.complete(job['id'],result)
                            del active[port]
                        job = self.claim(port)
                        if job is not None:
                            command = [str(PYTHON),'-B',str(ROOT/'tools/board.py'),'run',job['image'],'--port',port,
                                       '--evidence',str(Path(job['directory'])/'run'),
                                       '--process-record',str(Path(job['directory'])/'runner-process.json')]
                            for name,value in job['options'].items():
                                flag = '--'+name.replace('_','-')
                                if isinstance(value,bool):
                                    if value: command.append(flag)
                                elif isinstance(value,list):
                                    for item in value: command += [flag,str(item)]
                                elif value is not None:
                                    command += [flag,str(value)]
                            log = (Path(job['directory'])/'runner.log').open('a')
                            process = subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT)
                            with self.locked() as state:
                                entry = next(j for j in state['jobs'] if j['id']==job['id'])
                                entry['process'] = process_identity(process.pid)
                            active[port] = (process,job,log)
                    if once and not active:
                        status = self.status()['boards']
                        if not any(v['queued'] and not v['pause'] and port_idle(p) for p,v in status.items()):
                            return
                    time.sleep(.1 if active else .5)
            finally:
                # An interrupted daemon waits for its existing children. It never
                # kills a run, releases its device lock or launches a replacement.
                for process,job,log in active.values():
                    process.wait()
                    log.close()
                    path = Path(job['directory'])/'run/result.json'
                    if path.exists():
                        self.complete(job['id'],json.loads(path.read_text()))


def run_options(parser, submit=False):
    parser.add_argument('image', type=Path)
    parser.add_argument('--port', choices=('any',*PORTS) if submit else PORTS, required=True)
    parser.add_argument('--kind', choices=('cpu-return','gpu-return','npu','probe'), default='cpu-return', required=submit)
    parser.add_argument('--expect', required=submit, help='Existing file: exact bytes; otherwise UTF-8 substring.')
    parser.add_argument('--approved-by', choices=('coordinator',))
    parser.add_argument('--storage-write', action='store_true')
    parser.add_argument('--descriptor', type=Path, help='Hash-bound image metadata; defaults to IMAGE.board.json.')
    parser.add_argument('--template', type=Path, help='Typed-session bundle, or legacy bundle with board/ and assets/.')
    parser.add_argument('--host-tree', type=Path, help='Legacy GPU image source tree; generic images use this OS tree.')
    parser.add_argument('--input', action='append', type=Path, default=[])
    parser.add_argument('--fel', type=Path, default=dev.FEL)
    parser.add_argument('--rail-helper', type=Path, default=dev.RAIL_HELPER)
    parser.add_argument('--ddr-build', type=Path, default=CACHE/'build/a733-ddr')
    parser.add_argument('--ddr-restore', type=Path, default=CACHE/'build/a733-ddr-restore')
    parser.add_argument('--training-dir', type=Path, default=CACHE/'ddr-records')
    parser.add_argument('--expect-spec',type=Path,help=argparse.SUPPRESS)
    parser.add_argument('--reference-inventory',type=Path,help=argparse.SUPPRESS)
    parser.add_argument('--process-record',type=Path,help=argparse.SUPPRESS)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--queue', type=Path, default=CACHE/'board-queue')
    commands = parser.add_subparsers(dest='command',required=True)
    run = commands.add_parser('run')
    run_options(run)
    run.add_argument('--evidence',type=Path)
    submit = commands.add_parser('submit')
    run_options(submit,True)
    submit.add_argument('--worker',required=True)
    submit.add_argument('--hypothesis',required=True)
    submit.add_argument('--no-wait',action='store_true')
    commands.add_parser('status')
    watch = commands.add_parser('watch')
    watch.add_argument('entry',nargs='?',help='Entry ID to wait for, or display status on changes.')
    serve = commands.add_parser('serve')
    serve.add_argument('--once',action='store_true',help='Drain currently runnable entries and exit.')
    resume = commands.add_parser('resume')
    resume.add_argument('port',choices=PORTS)
    args = parser.parse_args(argv)
    if args.command=='run':
        if args.process_record:
            save(args.process_record,process_identity(os.getpid()))
        result = run_image(args)
        return 0 if result['outcome']=='returned' else 1
    queue = Queue(args.queue)
    if args.command=='submit':
        job = queue.submit(args)
        path = Path(job['result_path'])
        if not args.no_wait:
            while not path.exists():
                time.sleep(.2)
        print(path)
    elif args.command=='status':
        print(json.dumps(queue.status(),indent=2))
    elif args.command=='resume':
        queue.resume(args.port)
        print(f'{args.port} resumed')
    elif args.command=='serve':
        queue.serve(args.once)
    elif args.command=='watch':
        last = None
        while True:
            if args.entry:
                require(re.fullmatch(r'[0-9]+-[a-f0-9]{8}',args.entry),'Invalid entry ID.')
                path = queue.root/'entries'/args.entry/'result.json'
                if path.exists():
                    print(path)
                    return 0
            else:
                status = json.dumps(queue.status(),sort_keys=True,indent=2)
                if status!=last:
                    print(status,flush=True)
                    last = status
            time.sleep(.5)
    return 0


if __name__=='__main__':
    try:
        sys.exit(main())
    except (ValueError,OSError) as error:
        print(str(error),file=sys.stderr)
        sys.exit(2)
