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
import types
import uuid

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import board_images as images
from host_evidence import evidence_directory

ROOT, CACHE = images.ROOT, images.CACHE
PYTHON = CACHE / 'venv/bin/python3'
PORTS = ('3-1', '3-3')
# Pinned rail helper's linked __heap_end; probe.bin excludes its BSS/stacks.
# Derived from probe.elf SHA-256 ef63663020cf827f89811ef9cb60733a4965f70e518fe96ccd8821c59babfb5f.
# state.RAIL_HELPER_SHA256 binds this geometry to the reviewed binary.
RAIL_HELPER_DRAM = (0x41000000, 0x48300)
POST_RAIL_HELPER = CACHE/'evidence/sol-cpu-normal-return-1028/rails-800/probe.bin'
POST_RAIL_HELPER_SHA256 = 'f48a6be72b336c8990a124fd608bba5fddb5c8617ec62eac5642b2d50fb27c75'
# Linked __heap_end from ELF SHA-256 ae00ce1d4a1f82b77ab1e016dc1a08f795e56c9c6f4c92d166f6434d75590098.
POST_RAIL_HELPER_DRAM = (0x41000000, 0x47300)
OUTCOMES = ('returned', 'result-mismatch', 'watchdog-reset', 'no-return', 'refused-before-launch', 'host-error')
crumbs = images.load('board_crumbs', ROOT/'tools/a733-crumbs.py')
import board_state as state
import board_usb_power as usb_power
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


class DdrPowerAbort(BaseException):
    """Prevent a PL8 fault from entering the legacy DDR fallback."""


class Mismatch(ValueError):
    pass


class UnresolvedReturn(ValueError):
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
        self.dma_binding = None

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
                        self.usb_source_off('transport-recovery')
                        self.record['transport_recovered'] = True
                    except usb_power.PowerFault as cleanup_error:
                        self.record['cleanup_failure'] = str(cleanup_error)
                        self.journal()
                    except (NoReturn, TransportFailure):
                        self.quarantine(str(error))
                        raise
                else:
                    self.quarantine(str(error))
                raise
            except NoReturn as error:
                self.quarantine(str(error))
                raise
            except UnresolvedReturn as error:
                self.quarantine(str(error))
                raise
            finally:
                if (self.record.get('npu_dirty') or self.record.get('dram_dirty')) and not self.record.get('dram_restored'):
                    self.quarantine('DRAM ownership remains unresolved.')

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
        with Path(path).open('rb') as stream:
            os.fsync(stream.fileno())
        return data

    def upload(self, address, source, *, expected_sha256=None):
        data = Path(source).read_bytes()
        require(expected_sha256 is None or hashlib.sha256(data).hexdigest() == expected_sha256,
                'Saved DRAM changed before sealed restoration upload.')
        self.sequence += 1
        # A sealed source prevents a concurrent build changing upload bytes.
        base = images.load('board_sealed', ROOT/'tools/check-a733-smp.py')
        path = self.evidence/f'readback-{self.sequence}.bin'
        with base.sealed_source(data) as (sealed, fd):
            self.tx('write', hex(address), sealed, 'read', hex(address), len(data), path,
                    timeout=max(30, len(data)*2/100000+10), pass_fds=(fd,))
        if path.read_bytes() != data:
            raise Mismatch(f'Upload readback differs at {address:#x}; execution refused.')
        with path.open('rb') as stream:
            os.fsync(stream.fileno())

    def sanity(self):
        require('soc=00001903(A733)' in self.tx('version'), 'Target is not A733.')
        # readl uses SRAM scratch at 0x71000; preserve it before any ROM check.
        before = self.read(0x71000, 0x200, self.evidence/f'rom-scratch-{self.sequence}.bin')
        words = [int(x, 16) for x in self.tx(*[x for a in (0x20, 0x24, 0x28, 0x3c, 0x40)
                                             for x in ('readl', hex(a))]).split()]
        require(words == [0xeb00008f, 0xe3a00911, 0xe3a0193d, 0xe3a0605c, 0xea00004d], 'Forced-FEL ROM entry differs.')
        self.upload(0x71000, images.write(self.evidence/f'rom-scratch-restore-{self.sequence}.bin', before))

    def usb_source_off(self, phase, enforce=True):
        usb_power.observe(self, phase)
        if enforce:
            usb_power.enforce(self)

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
            self.observe_return_power('invalid-return-report')
            raise RuntimeError('Invalid watchdog return report: '+str(error)) from error
        self.confirm_disarmed(after)
        self.record['watchdog_return'] = {'before': before, 'after': after, 'words': words}
        self.journal()
        self.observe_return_power('native-return')
        return before,after,words

    def observe_return_power(self, phase):
        # Preserve the copied WDT/breadcrumb origin before a cleanup failure.
        # The caller classifies these bytes before any DRAM result read.
        try:
            self.usb_source_off(phase, enforce=False)
        except (usb_power.PowerFault, TransportFailure) as error:
            self.record['usb_vbus_cleanup_failure'] = str(error)
            self.record['cleanup_failure'] = str(error)
            if isinstance(error, TransportFailure):
                self.quarantine(str(error))
            self.journal()

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
        self.usb_source_off('fresh-fel')
        self.sanity()
        self.record['fresh_fel'] = True

    def finish_pending(self):
        """A host error cannot release an armed run before its reset deadline."""
        if self.armed and not self.failed:
            self.recovering = True
            self.wait(max(0, self.recovery_deadline-self.now()))
            self.disarm()
            self.usb_source_off('pending-return')

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
            self.usb_source_off('ddr-complete')
        except (TransportFailure, NoReturn, usb_power.PowerFault):
            raise
        except DdrPowerAbort as error:
            raise usb_power.PowerFault(str(error)) from error
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
            try:
                self.usb_source_off('ddr-new-instance')
            except usb_power.PowerFault as error:
                raise DdrPowerAbort(str(error)) from error
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
            if 'exe' in commands:
                try:
                    self.usb_source_off('ddr-helper-return')
                except usb_power.PowerFault as error:
                    raise DdrPowerAbort(str(error)) from error
        except TransportFailure as error:
            record['transport_failed'] = True
            save_record()
            # A failed request to the old instance terminates that instance.
            # Do not let run-ddr's legacy fallback retry it.
            raise DdrAbort(str(error)) from error
        save_record()
        return result

    def rails(self, phase='rails'):
        post = phase == 'restore-rails'
        if post:
            require(self.record.get('normal_return') and self.record.get('native_retirement_validated') and
                    self.record.get('dram_restored') and not self.record.get('dram_lost'),
                    'Post-ROM rail helper requires validated native retirement and restored DRAM.')
        helper = POST_RAIL_HELPER if post else self.args.rail_helper
        expected = POST_RAIL_HELPER_SHA256 if post else state.RAIL_HELPER_SHA256
        reservation = POST_RAIL_HELPER_DRAM if post else RAIL_HELPER_DRAM
        evidence = self.evidence/phase
        evidence.mkdir(parents=True, exist_ok=True)
        data = helper.read_bytes()
        require(hashlib.sha256(data).hexdigest() == expected, 'Rail helper binary differs from the pinned one.')
        oscillator = int(self.tx('readl','0x07090160'),16)
        require((oscillator>>14)&3==2 and oscillator&0x10000002==0x10000002,
                'Rail helper requires the enabled raw 26 MHz oscillator.')
        require(len(data) <= reservation[1], 'Rail helper exceeds its linked reservation.')
        regions = [reservation, (0x42000000, 16384)]
        backups = images.backup_dram(self, regions, phase)
        self.record.update(dram_dirty=True, dram_restored=False)
        self.record.setdefault('rail_dram_backups', {})[phase] = backups
        self.journal()
        self.upload(0x41000000, images.write(evidence/'rail-helper.bin',data))
        self.upload(0x42000000, images.write(evidence/'rail-blank.bin', bytes(16384)))
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
        run = {'return': self.record['rail_return'], 'evidence': str(evidence)}
        self.record.setdefault('rail_runs', {})[phase] = run
        if not (before&0xffff==0xb1 and not after&1 and 0<=elapsed<14):
            self.record.update(dram_lost=True,setup_watchdog_reset=True)
            raise ValueError('Rail helper possible watchdog reset; no DRAM result read; rails were not normalized.')
        usb_power.enforce(self)
        data = self.read(0x42000000, 16384, evidence/'rail-result.bin')
        values = struct.unpack_from('<16Q', data)
        require(values[3] <= 16384-257, 'Invalid rail helper log length.')
        log = data[256:256+values[3]].decode(errors='replace')
        (evidence/'rail.log').write_text(log)
        # The helper performs all boot-clock, speed-bin and PMIC readbacks.
        self.record['rails'] = images.post_rail_result(values, log) if post else images.rail_result(values, log)
        run['image_sha256'] = expected
        run['rails'] = self.record['rails']
        images.restore_dram(self, backups, regions)
        self.record.update(dram_restored=True, dram_dirty=False)
        run['dram_restored'] = True
        self.journal()

    def live_state(self, finite_npu=False):
        words = images.load('npu_live_words', (self.args.host_tree or ROOT)/'apps/npu-finite/development.py').WORDS if finite_npu else state.WORDS
        values = [int(v,16) for v in self.tx(*[x for a in words for x in ('readl', hex(a))]).split()]
        require(len(values) == len(words), 'Live state word count differs.')
        return {'identity': self.identity, 'port': self.args.port, 'words': {hex(a):v for a,v in zip(words,values)}}

    def npu_on_state(self):
        # Match power_domains level()/coherent(): gated PPU reads only, no
        # engine access and no request that could power an OFF domain ON.
        proof = {'source': 'fresh read-only FEL under the port lock', 'status': 'refused',
                 'raw': [], 'words': {}}
        self.record['npu_on_admission'] = proof
        def read_words(addresses):
            raw = self.tx(*[item for address in addresses for item in ('readl', hex(address))])
            proof['raw'].append({'addresses': [hex(a) for a in addresses], 'stdout': raw})
            self.journal()
            values = [int(value, 16) for value in raw.split()]
            require(len(values) == len(addresses), 'Incomplete NPU entry state readback.')
            for address, value in zip(addresses, values):
                require(0 <= value <= 0xffffffff, 'Invalid NPU entry state readback.')
                proof['words'][hex(address)] = value
            self.journal()
        read_words([0x070101ac])
        require(proof['words']['0x70101ac'] & 1, 'PCK clock gate is closed; PPU access refused.')
        read_words([0x020025c0, 0x020025e0])
        require(all(proof['words'][hex(address)] & mask == mask
                    for address, mask in images.NPU_ENTRY_GATES.items()),
                'NPU fabric gate is closed; PPU access refused.')
        read_words(images.NPU_PCK4)
        images.validate_npu_on(proof['words'])
        proof['status'] = 'ON'
        self.journal()
        return proof['words']

    def prepare(self, info):
        if 'usb_vbus_probe_profile' in info.get('symbols', {}):
            usb_power.admit_diagnostic(self)
        if info.get('dma'):
            require(self.dma_binding is not None, 'DMA runner has no original-instance admission.')
        live = {} if info.get('native_probe') or self.args.kind=='probe' else self.live_state(info.get('finite_npu',False))
        if images.npu_on_required(info):
            live['words'].update(self.npu_on_state())
        return images.prepare(self.args, info, live, self.evidence/'bundle', dma_binding=self.dma_binding)

    def launch(self, plan):
        if plan.get('format') == 'generic':
            images.validate_uploads(plan)
            plan['dram_backups'] = images.backup_dram(self, plan['preserve_regions'])
            self.record['dram_backups'] = plan['dram_backups']
            self.record.update(dram_restored=False, dram_dirty=True)
            self.journal()
        if plan.get('format') == 'npu':
            plan['finite'].require_entry_state(self.tx)
            backup = self.evidence/'npu-DRAM-before.bin'
            self.read(*plan['finite'].DRAM, backup)
            self.record['dram_restored'] = False
            self.record['npu_dirty'] = True
            plan.update(dram_backup=backup, dram_backup_sha256=images.sha(backup))
            self.record['dram_backup_sha256'] = plan['dram_backup_sha256']
            self.journal()
        for address, source in plan['uploads']:
            self.upload(address, source)
        if plan.get('format') == 'npu':
            self.sanity()
            plan['finite'].require_entry_state(self.tx)
        self.record['launch_attempted'] = True
        self.journal()
        self.arm(plan['watchdog']['mode'])
        self.recovery_deadline = self.now()+plan['watchdog']['session_seconds']+crumbs.interval(plan['watchdog']['mode'])+10
        self.tx(*plan['launch'])
        self.wait(plan['watchdog']['session_seconds'] + crumbs.interval(plan['watchdog']['mode']) + 10)
        before, after, words = self.returned()
        elf = self.args.image if plan.get('format') == 'npu' or self.args.image.suffix == '.elf' else None
        decoded = crumbs.decode(words, plan['epoch'], elf)
        self.record['breadcrumbs'] = decoded
        save(self.evidence/'breadcrumbs.json', decoded)
        require(not after & 1, 'WDT0 did not disarm after return.')
        self.record['rom_returned'] = True
        if before & 1:
            self.record['dram_lost'] = True
            return 'watchdog-reset'
        if not (decoded['session'] and decoded.get('last',{}).get('stage') == crumbs.RETURN_ROM and
                decoded['last']['value'] == 0 and not decoded.get('fault') and not decoded.get('board_failure')):
            raise UnresolvedReturn('Return lacks the matching RETURN_ROM breadcrumb.')
        self.record['normal_return'] = True
        usb_power.enforce(self)
        return 'returned'

    def capture(self, plan):
        try:
            return images.capture(self, plan, self.args)
        except BaseException as error:
            if plan.get('format') in ('npu', 'generic') and not self.record.get('dram_restored'):
                self.quarantine('Native retirement or DRAM restoration unverified: '+str(error))
            if isinstance(error, ValueError):
                raise Mismatch(str(error)) from error
            raise


def expectation(value):
    if value is None:
        return None
    path = Path(value)
    if path.is_file():
        raw = path.read_bytes()
        try:
            spec = json.loads(raw)
        except (UnicodeDecodeError, json.JSONDecodeError):
            spec = None
        if isinstance(spec, dict) and isinstance(spec.get('schema'), str) and spec['schema'].startswith('sunstone-board-expect-'):
            require(set(spec) == {'schema', 'words'} and spec['schema'] == 'sunstone-board-expect-u32-v1',
                    'Unknown binary expectation schema.')
            return validate_expectation({'mode': 'u32le', 'words': spec['words']})
        return {'mode': 'bytes', 'hex': raw.hex()}
    return {'mode': 'text', 'text': value}


def validate_expectation(expected):
    """Validate a file-derived or frozen queue expectation before device access."""
    require(isinstance(expected, dict), 'Invalid frozen expectation.')
    mode = expected.get('mode')
    if mode == 'bytes':
        require(set(expected) == {'mode', 'hex'} and isinstance(expected['hex'], str),
                'Invalid exact-byte expectation.')
        bytes.fromhex(expected['hex'])
    elif mode == 'text':
        require(set(expected) == {'mode', 'text'} and isinstance(expected['text'], str),
                'Invalid text expectation.')
    else:
        require(mode == 'u32le' and set(expected) == {'mode', 'words'},
                'Unknown frozen expectation mode.')
        words = expected['words']
        require(isinstance(words, list) and 1 <= len(words) <= 16384,
                'Binary expectation must name every 32-bit word.')
        for word in words:
            require(type(word) is int and 0 <= word <= 0xffffffff or
                    isinstance(word, dict) and set(word) == {'min', 'max'} and
                    all(type(word[k]) is int for k in ('min', 'max')) and
                    0 <= word['min'] <= word['max'] <= 0xffffffff,
                    'Invalid fixed word or inclusive range in binary expectation.')
    return expected


def matches_expectation(expected, payload):
    """Compare every binary field while allowing declared hardware timings."""
    if expected['mode'] == 'bytes':
        return payload == bytes.fromhex(expected['hex'])
    if expected['mode'] == 'text':
        return expected['text'] in payload.decode(errors='replace')
    require(expected['mode'] == 'u32le', 'Unknown frozen expectation mode.')
    words = expected['words']
    if len(payload) != 4 * len(words):
        return False
    actual = struct.unpack('<' + 'I' * len(words), payload)
    return all(got == want if type(want) is int else want['min'] <= got <= want['max']
               for got, want in zip(actual, words))


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
        if args.kind == 'npu' and 'ss_result_record' not in images.symbols(args.image):
            finite = images.load('reference_npu_irq', tree/'apps/npu-interrupt/host.py').finite
            paths.update(tree/name for name in finite.HOST_SOURCES)
            paths.update(tree/name for name in ('apps/npu-finite/development.py','tools/vendor-mbus.py'))
    paths.update((args.fel,args.rail_helper))
    if args.restore_rails:
        paths.update((POST_RAIL_HELPER, POST_RAIL_HELPER.parent/'input-identities.json',
                      POST_RAIL_HELPER.parent/'build/probe.elf'))
    if args.kind == 'npu':
        paths.add(args.objcopy)
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
    evidence = evidence_directory(evidence)
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
        if args.kind == 'npu' and 'ss_result_record' not in images.symbols(args.image):
            manifest, session = images.finite_npu_files(args)
            shutil.copy2(manifest, evidence/'manifest.json')
            shutil.copy2(session, evidence/'npu-session-template.bin')
            args.manifest = evidence/'manifest.json'
            args.records = evidence/'npu-session-template.bin'
            finite = images.load('preflight_npu_irq',(args.host_tree or ROOT)/'apps/npu-interrupt/host.py').finite
            finite.original.session_profile(args.records.read_bytes(), (456,224,232,72,96,128))
        args.image = frozen
        if args.reference_inventory:
            verify_references(json.loads(args.reference_inventory.read_text()))
        helper_only = args.kind == 'rail-helper'
        dma_binding = None
        if helper_only:
            require(result['image_sha256'] == state.RAIL_HELPER_SHA256,
                    'Rail helper binary differs from the pinned one.')
            args.rail_helper = frozen
            info = None
        else:
            info = images.inspect(frozen, args.kind, descriptor,args.host_tree)
            images.policy(info, args)
            if info['entry_guard'] and args.restore_rails:
                require(POST_RAIL_HELPER.is_file() and images.sha(POST_RAIL_HELPER) == POST_RAIL_HELPER_SHA256,
                        'Approved post-ROM rail helper is absent or changed; refused before device access.')
            dma_binding = images.dma_admission(args, info)
            application_inputs(args, info, evidence)
            require(getattr(args, 'expect_result', None) is None or getattr(args, 'decoder', None) is not None,
                    '--expect-result requires a result decoder.')
        expected = validate_expectation(json.loads(args.expect_spec.read_text())) if args.expect_spec else expectation(args.expect)
        if expected is not None:
            save(evidence/'expect.json', expected)
        owner = backend(args, evidence, result)
        with owner.locked():
            try:
                timed('identity', owner.sanity)
                timed('usb-source-off', lambda: owner.usb_source_off('admission'))
                if dma_binding is not None:
                    owner.dma_binding = timed('dma-identity', lambda: images.bind_dma_identity(dma_binding, args, owner.identity))
                timed('reset', owner.reset)
                timed('dram', owner.dram)
                if helper_only or info['entry_guard'] or args.kind == 'npu':
                    timed('rails', owner.rails)
                if helper_only:
                    result['outcome'] = 'returned'
                    result['return_report'] = result['rails']
                    return finish_result(result, evidence, started, stage)
                plan = timed('prepare', lambda: owner.prepare(info))
                outcome = timed('launch', lambda: owner.launch(plan))
                result['outcome'] = outcome
                if outcome == 'returned':
                    captured = timed('capture', lambda: owner.capture(plan))
                    payload = captured.pop('payload')
                    (evidence/'output.bin').write_bytes(payload)
                    result['return_report'] = captured['report']
                    result['output_sha256'] = hashlib.sha256(payload).hexdigest()
                    if info['entry_guard'] and getattr(args, 'restore_rails', False):
                        if plan.get('format') == 'generic':
                            require(result.get('native_retirement_validated') and result.get('dram_restored'),
                                    'Rail restoration requires native retirement and restored DRAM.')
                        after = timed('restore-state', owner.live_state)
                        save(evidence/'state-after.json', after)
                        require(not state.boot_clocks(after['words']),
                                'CPU clocks differ from boot words before rail restoration.')
                        # Mandatory retirement and RAM restoration precede optional result validation.
                        timed('restore-rails', lambda: owner.rails('restore-rails'))
                        result['rails_restored'] = True
                    matched = captured['valid']
                    if expected:
                        matched &= matches_expectation(expected, payload)
                    if not matched:
                        raise Mismatch('Returned result differs from expectation.')
                    if getattr(args, 'decoder', None):
                        decoder = load_decoder(args)
                        try:
                            decoded = decoder.decode(payload)
                        except (ValueError, struct.error) as error:
                            raise Mismatch('Invalid decoded result: '+str(error)) from error
                        result['decoded'] = decoded
                        if hasattr(decoder, 'failures'):
                            failures = decoder.failures(decoded)
                            if failures:
                                raise Mismatch('Decoder reported failures: '+str(failures))
                        if getattr(args, 'expect_result', None) is not None or hasattr(decoder, 'expected'):
                            values = [p.read_bytes() for p in args.input]
                            if not values:
                                values = [p.read_bytes() for p in sorted((evidence/'bundle').glob('input-*.bin'))]
                            wanted = args.expect_result if getattr(args, 'expect_result', None) is not None else decoder.expected(values)
                            if decoded != wanted:
                                raise Mismatch('Decoded result differs from expectation.')
                else:
                    decoded = result.get('breadcrumbs',{})
                    fault = decoded.get('fault',{})
                    last = decoded.get('last',{})
                    origin = decoded.get('board_failure')
                    result['first_failure'] = dict(origin, native_site=origin['site'],
                        site=f"{origin['group']}:{origin['site']}:{origin['caller']}",
                        detail=decoded.get('summary','Watchdog reset')) if origin else {
                            'site': fault.get('elr') or last.get('name','before-entry'),
                            'detail': decoded.get('summary','Watchdog reset')}
                if owner.journal_error is not None:
                    raise owner.journal_error
            except BaseException as error:
                # Wait out any armed helper/image if a host exception interrupted
                # the launch path. Never send another request to a failed instance.
                if owner.armed and not owner.failed and not isinstance(error, NoReturn):
                    try:
                        owner.finish_pending()
                    except usb_power.PowerFault as cleanup_error:
                        result['cleanup_failure'] = str(cleanup_error)
                        owner.journal()
                raise
    except NoReturn as error:
        result['outcome'] = 'no-return'
        result['first_failure'] = {'site': stage, 'detail': str(error)}
    except usb_power.PowerFault as error:
        result['outcome'] = 'result-mismatch' if result.get('launch_attempted') else 'refused-before-launch'
        result['first_failure'] = result.get('usb_vbus_fault', {'site': stage, 'detail': str(error)})
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
    return finish_result(result, evidence, started, stage)


def finish_result(result, evidence, started, stage):
    result['stage'] = stage
    result['durations']['total'] = round(time.monotonic()-started,6)
    # Raw transaction evidence has its own record; result.json stays compact.
    summary = {k:v for k,v in result.items() if k != 'transactions'}
    save(evidence/'result.json', summary)
    print(f"{summary['port']} {summary['kind']} {summary.get('image_sha256','unreadable')[:12]} {summary['outcome']} {evidence/'result.json'}", flush=True)
    return summary


def check_image(args):
    """Apply launch admission and prepare uploads without constructing a USB owner."""
    descriptor = args.descriptor or Path(str(args.image)+'.board.json')
    manifest = None
    if args.kind == 'npu' and 'ss_result_record' not in images.symbols(args.image):
        manifest = json.loads(images.finite_npu_files(args)[0].read_bytes())
    info = images.inspect(args.image, args.kind,
                          json.loads(descriptor.read_text()) if descriptor.exists() else None,
                          args.host_tree, manifest=manifest)
    images.policy(info, args)
    evidence = evidence_directory(args.evidence)
    application_inputs(args, info, evidence)
    live = json.loads(args.state.read_text()) if args.state else {}
    dma_binding = images.dma_admission(args, info, live if live.get('identity') is not None else None)
    require(info.get('native_probe') or live.get('words'),
            'Offline application preparation requires --state with captured words.')
    plan = images.prepare(args, info, live, evidence/'bundle', dma_binding=dma_binding)
    report = {'admitted': True, 'physical_access': False, 'image_sha256': images.sha(args.image),
              'watchdog': plan['watchdog'],
              'uploads': [{'address': a, 'file': str(p), 'sha256': images.sha(p)} for a, p in plan['uploads']]}
    report.update({k: plan[k] for k in ('epoch', 'generation', 'lease', 'marker') if k in plan})
    save(evidence/'admission.json', report)
    print(json.dumps(report, indent=2))
    return 0


def application_inputs(args, info, evidence):
    """Freeze consumed inputs and validate their manifest bounds before device access."""
    if not info.get('generic'):
        return
    host = images.load('runner_application_manifest', ROOT/'apps/os/host.py')
    manifest = host.image_manifest(args.image)['manifest']
    if getattr(args, 'decode_application', False) and not args.decoder:
        candidate = ROOT/'apps'/manifest['name']/'host.py'
        if candidate.is_file():
            args.decoder = candidate
    decoder = None
    if args.decoder:
        args.decoder_origin = getattr(args, 'decoder_origin', None) or args.decoder.resolve()
        target = evidence/'decoder.py'
        target.write_bytes(args.decoder.read_bytes())
        args.decoder = target
        decoder = load_decoder(args)
        require(callable(getattr(decoder, 'decode', None)), 'Decoder requires decode(payload).')
        if hasattr(decoder, 'BINDING'):
            binding = decoder.BINDING
            require(isinstance(binding, dict) and binding.get('image_sha256') == images.sha(args.image),
                    'Frozen decoder BINDING must name the selected image SHA-256.')
    values = [p.read_bytes() for p in args.input]
    if not values and getattr(args, 'decode_application', False) and decoder and hasattr(decoder, 'inputs'):
        values = decoder.inputs()
    if not values and manifest['name'] == 'hello-compute':
        values = [b'SunstoneOS board control\n']
    if values:
        host.input_facts(manifest, values)
        args.input = [images.write(evidence/f'input-{i}.bin', value) for i, value in enumerate(values)]


def load_decoder(args):
    """Execute frozen decoder bytes while retaining its source-relative resource paths."""
    origin = getattr(args, 'decoder_origin', None) or args.decoder
    module = types.ModuleType('board_application_decoder')
    module.__file__ = str(origin)
    exec(compile(args.decoder.read_bytes(), str(origin), 'exec'), module.__dict__)
    return module


def development(argv=None):
    """Translate the development CLI; all admission and execution use this runner."""
    parser = argparse.ArgumentParser(description='Compatibility commands for tools/board.py')
    parser.add_argument('kind', choices=('cpu-return', 'gpu-return', 'npu', 'usb-class', 'rail-helper'))
    parser.add_argument('--image', type=Path)
    parser.add_argument('--input', type=Path, action='append', default=[])
    parser.add_argument('--decoder', type=Path)
    parser.add_argument('--records', type=Path)
    parser.add_argument('--template', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--descriptor', type=Path)
    parser.add_argument('--approved-by', choices=('coordinator',))
    parser.add_argument('--host-tree', type=Path)
    parser.add_argument('--objcopy', type=Path)
    parser.add_argument('--expect-result', type=json.loads)
    parser.add_argument('--port', default=os.environ.get('SUNSTONE_USB_PORT'), choices=PORTS)
    parser.add_argument('--evidence', type=Path)
    parser.add_argument('--state', type=Path)
    parser.add_argument('--check-only', action='store_true')
    parser.add_argument('--force', action='store_true')
    parser.add_argument('--normalize-rails', action=argparse.BooleanOptionalAction)
    parser.add_argument('--restore-rails', action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument('--rail-helper', type=Path, default=state.RAIL_HELPER)
    parser.add_argument('--fel', type=Path, default=state.FEL)
    args = parser.parse_args(argv)
    if args.force or args.normalize_rails is False:
        print('Legacy force/skip-normalize options cannot bypass runner admission.', file=sys.stderr)
    offline = args.check_only or args.state is not None
    if args.kind == 'rail-helper' and offline:
        parser.error('rail-helper requires a live runner session; no --state or --check-only')
    if not offline and not args.port:
        parser.error('select --port or SUNSTONE_USB_PORT')
    image = args.image or (args.rail_helper if args.kind == 'rail-helper' else state.CONTROL_IMAGE)
    evidence = args.evidence or CACHE/'board-queue/runs'/f'{time.time_ns()}-development'
    command = ['check' if offline else 'run', str(image), '--kind', args.kind, '--evidence', str(evidence),
               '--fel', str(args.fel), '--rail-helper', str(args.rail_helper)]
    if args.port:
        command += ['--port', args.port]
    if args.state:
        command += ['--state', str(args.state)]
    if args.kind == 'cpu-return':
        command += ['--records', str(args.records or state.RECORDS)]
    elif args.records:
        command += ['--records', str(args.records)]
    for name in ('template', 'manifest', 'descriptor', 'host_tree', 'objcopy', 'approved_by'):
        if getattr(args, name):
            command += ['--'+name.replace('_','-'), str(getattr(args, name))]
    if args.expect_result is not None:
        command += ['--expect-result', json.dumps(args.expect_result)]
    if args.restore_rails and args.kind not in ('npu', 'usb-class'):
        command += ['--restore-rails']
    if args.decoder:
        command += ['--decoder', str(args.decoder)]
    command += ['--decode-application']
    for path in args.input:
        command += ['--input', str(path)]
    return main(command)


def legacy_run(args, kind='probe', image=None, storage_write=False):
    """Route a historical host's launch subcommand through runner admission."""
    bundle = getattr(args, 'bundle', None)
    if image is None:
        directory = Path(bundle)
        image = directory/'board/image.elf' if (directory/'board/image.elf').is_file() else directory/'image.elf'
    command = ['run', str(image), '--kind', kind,
               '--port', os.environ.get('SUNSTONE_USB_PORT', ''),
               '--fel', str(args.fel), '--evidence', str(args.evidence)]
    if bundle:
        command += ['--template', str(bundle)]
    if storage_write:
        command += ['--storage-write']
    if getattr(args, 'expect', None):
        command += ['--expect', args.expect]
    return main(command)


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
        if args.decoder and not args.decoder_origin:
            args.decoder_origin = args.decoder.resolve()
        data = source.read_bytes()
        image = directory/('image.elf' if data[:4] == b'\x7fELF' else 'image.bin')
        image.write_bytes(data)
        if args.decode_application:
            descriptor = args.descriptor or Path(str(source)+'.board.json')
            manifest = None
            if args.kind == 'npu' and 'ss_result_record' not in images.symbols(source):
                manifest = json.loads(images.finite_npu_files(args)[0].read_bytes())
            info = images.inspect(source, args.kind,
                                  json.loads(descriptor.read_text()) if descriptor.exists() else None,
                                  args.host_tree, manifest=manifest)
            application_inputs(args, info, directory)
        # Freeze all caller-controlled launch inputs before queue visibility.
        options = {}
        for name in ('kind','approved_by','storage_write','host_tree','fel','rail_helper','ddr_build','ddr_restore','training_dir','restore_rails','decode_application','objcopy'):
            value = getattr(args,name)
            options[name] = str(value.resolve()) if isinstance(value,Path) else value
        for name in ('records', 'decoder', 'manifest'):
            path = getattr(args, name, None)
            if path:
                target = directory/(name+path.suffix)
                if path.resolve() != target.resolve():
                    shutil.copy2(path, target)
                options[name] = str(target)
        if getattr(args, 'decoder_origin', None):
            options['decoder_origin'] = str(args.decoder_origin)
        if args.expect_result is not None:
            options['expect_result'] = json.dumps(args.expect_result)
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
        if args.kind not in ('probe', 'usb-class', 'rail-helper'):
            if args.kind == 'npu' and 'ss_result_record' not in images.symbols(source):
                manifest, session = images.finite_npu_files(args)
                destination = directory/'template'
                destination.mkdir()
                shutil.copy2(session, destination/'session.bin')
                shutil.copy2(manifest, destination/'manifest.json')
            elif args.kind=='gpu-return':
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
                if (template/'dma-authority.json').is_file():
                    shutil.copy2(template/'dma-authority.json', destination/'dma-authority.json')
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
        with self.locked() as state:
            state['pauses'].pop(port,None)
            path = self.root/f'paused-{port}.json'
            if path.exists():
                archive = self.root/'resumed'
                archive.mkdir(exist_ok=True)
                path.rename(archive/f'{time.time_ns()}-{port}.json')
            self.event('resumed',port=port)

    def recover(self, exclude=()):
        """The daemon collects stopped orphans; live runners retain their claims."""
        exclude = set(exclude)
        with self.locked() as state:
            interrupted = [dict(j) for j in state['jobs'] if j['state']=='running' and j['id'] not in exclude]
        for job in interrupted:
            port = job['assigned_port']
            path = Path(job['directory'])/'run/result.json'
            lease = Path(job['directory'])/'runner-process.json'
            identity = json.loads(lease.read_text()) if lease.exists() else job.get('process')
            alive = identity is not None and process_alive(identity)
            if alive:
                continue
            if path.exists() and port_idle(port):
                self.complete(job['id'],json.loads(path.read_text()))
            else:
                with self.locked() as state:
                    self.pause(state,port,'Daemon interrupted during a run; inspect the runner and port lock.')
                if port_idle(port) and identity is not None:
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
    parser.add_argument('--kind', choices=('cpu-return','gpu-return','npu','probe','usb-class','rail-helper'), default='cpu-return', required=submit)
    parser.add_argument('--expect', required=submit, help='File: exact bytes or sunstone-board-expect-u32-v1 JSON; otherwise UTF-8 substring.')
    parser.add_argument('--expect-result', type=json.loads, help='Required decoded JSON result; needs a result decoder.')
    parser.add_argument('--manifest', type=Path, help='Exact finite NPU build manifest.')
    parser.add_argument('--objcopy', type=Path, default=Path('/usr/bin/llvm-objcopy-21'))
    parser.add_argument('--approved-by', choices=('coordinator',))
    parser.add_argument('--storage-write', action='store_true')
    parser.add_argument('--descriptor', type=Path, help='Hash-bound image metadata; defaults to IMAGE.board.json.')
    parser.add_argument('--restore-rails', action='store_true', help='Restore guarded CPU rails after a validated normal return.')
    parser.add_argument('--decoder', type=Path, help='Application payload decoder with optional expected(inputs).')
    parser.add_argument('--decoder-origin', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('--decode-application', action='store_true', help='Use the application decoder and its default inputs when available.')
    parser.add_argument('--records', type=Path, help='Board, thermal and CPU session records for a generic image.')
    parser.add_argument('--template', type=Path, help='Typed-session bundle, or legacy bundle with board/ and assets/.')
    parser.add_argument('--host-tree', type=Path, help='Host source tree for image geometry, session preparation and return validation.')
    parser.add_argument('--input', action='append', type=Path, default=[])
    parser.add_argument('--fel', type=Path, default=state.FEL)
    parser.add_argument('--rail-helper', type=Path, default=state.RAIL_HELPER)
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
    check = commands.add_parser('check')
    run_options(check)
    check.set_defaults(kind='probe')
    check._option_string_actions['--port'].required = False
    # Offline checks use captured words only; the port selects metadata, never a device.
    check.add_argument('--state', type=Path)
    check.add_argument('--evidence', type=Path, required=True)
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
    if args.command=='check':
        return check_image(args)
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
