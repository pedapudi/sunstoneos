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
import host_priority
import host_space
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
DRAM_UNRESOLVED = 'DRAM ownership remains unresolved.'
OUTCOMES = ('returned', 'failure-return', 'watchdog-reset', 'no-return', 'refused-before-launch', 'host-error')
crumbs = images.load('board_crumbs', ROOT/'tools/a733-crumbs.py')
import board_state as state
import board_usb_power as usb_power
require = images.require


def wifi_module():
    """tools/wifi-credential.py: the one definition of the stored credential, its fingerprint and the board record."""
    return images.load('wifi_credential', ROOT/'tools/wifi-credential.py')


def load_wifi(name, fingerprint=None):
    """The stored credential NAME, after its directory and file modes pass and, when given, its fingerprint matches.

    The refusal text names the credential and the fingerprints, never a value."""
    wifi = wifi_module()
    try:
        entry = wifi.load(name)
    except wifi.CredentialError as error:
        raise ValueError('Wi-Fi credential %r refused: %s' % (name, error)) from None
    now = wifi.fingerprint(entry['pmk'])
    require(fingerprint is None or now == fingerprint,
            'Wi-Fi credential %r changed since submit (fingerprint %s then, %s now); resubmit the run.' % (name, fingerprint, now))
    return entry


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


def heading_anchors(text):
    """GitHub heading anchors of a Markdown text: ATX headings outside fences, duplicates numbered."""
    anchors, seen, fence = [], {}, None
    for line in text.splitlines():
        marker = re.match(r' {0,3}(`{3,}|~{3,})', line)
        if marker:
            if fence is None:
                fence = marker.group(1)[0]
            elif marker.group(1)[0] == fence:
                fence = None
            continue
        heading = None if fence else re.fullmatch(r' {0,3}#{1,6}[ \t]+(.*?)(?:[ \t]+#+)?[ \t]*', line)
        if not heading:
            continue
        label = re.sub(r'<[^>]+>', '', re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', heading.group(1)))
        slug = re.sub(r'[^\w\- ]', '', label.lower()).replace(' ', '-')
        count = seen.get(slug, 0)
        seen[slug] = count + 1
        anchors.append(slug if count == 0 else f'{slug}-{count}')
    return anchors


def check_reference(args):
    """Validate the --reference of a submission; return the record kept in the entry and the result."""
    value = getattr(args, 'reference', None)
    require(bool(value), 'A submission requires --reference PATH#ANCHOR naming the docs heading that derives the vendor sequence and the diff against it; '
                         '--reference none needs --approved-by coordinator and --reference-reason.')
    if value == 'none':
        reason = (getattr(args, 'reference_reason', None) or '').strip()
        require(getattr(args, 'approved_by', None) == 'coordinator' and bool(reason),
                '--reference none requires --approved-by coordinator and a nonempty --reference-reason.')
        return {'none': True, 'approved_by': 'coordinator', 'reason': reason}
    path, separator, anchor = value.partition('#')
    anchor = anchor.lstrip('#')
    require(bool(separator and path and anchor), '--reference must read PATH#ANCHOR.')
    tree = (getattr(args, 'host_tree', None) or ROOT).resolve()
    target = (Path(path) if Path(path).is_absolute() else tree/path).resolve()
    require(target.is_relative_to(tree/'docs'), f'Reference {path} is not under docs/ in the host tree.')
    require(target.is_file(), f'Reference file {path} does not exist.')
    relative = target.relative_to(tree).as_posix()
    tracked = subprocess.run(['git', '-C', str(tree), 'ls-files', '--error-unmatch', '--', relative],
                             capture_output=True, text=True)
    require(tracked.returncode == 0, f'Reference file {relative} is not tracked in the host tree.')
    anchors = heading_anchors(target.read_text(errors='replace'))
    require(anchor in anchors, f'Reference anchor #{anchor} matches no heading in {relative}.')
    return {'path': relative, 'anchor': anchor, 'sha256': images.sha(target)}


def reference_text(record):
    """The one-line form kept in evidence records."""
    if not record:
        return None
    return f"none: {record['reason']}" if record.get('none') else f"{record['path']}#{record['anchor']}"


class Live:
    """The only FEL owner. Every request verifies the selected current instance."""
    def __init__(self, args, evidence, record, access=None, process=None, now=None, sleep=None, elapsed_now=None,
                 wall_now=None, instance_created=None):
        self.args, self.evidence, self.record = args, evidence, record
        self.access = access or images.load('board_access', ROOT/'tools/usb-access.py')
        self.process = process or subprocess.run
        self.now, self.sleep = now or time.monotonic, sleep or time.sleep
        self.elapsed_now = elapsed_now or (now if now is not None else lambda: time.clock_gettime(time.CLOCK_BOOTTIME))
        # The wall clock stamps what the host did; instance_created(node) is the kernel's creation time of the device
        # node of the adopted FEL instance (the same clock), or None when it cannot be read.
        self.wall_now = wall_now or time.time
        self.instance_created = instance_created or self.node_created
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
        self.credential = None  # bytearray: the Wi-Fi record, held in this process only and zeroed once delivered

    @staticmethod
    def watchdog_reset_seen(before):
        """An image disarms WDT0 on its only normal return, so a returned instance with WDT0 still enabled was reset by it.
        (The 40 recorded watchdog-reset results of 2026-11-02 read the mode as 1; the normal returns read 0.)"""
        return bool(before & 1)

    @staticmethod
    def node_created(node):
        """When the kernel created the device node of a USB instance (statx birth time, which devtmpfs sets when the node
        is made and which later permission and ACL changes by udev or logind do not move; the modification time does move),
        in the wall clock, or None. Metadata only: the node is not opened."""
        import ctypes
        buffer = ctypes.create_string_buffer(256)
        try:
            libc = ctypes.CDLL(None, use_errno=True)
            if libc.statx(-100, os.fsencode(str(node)), 0, 0x800, buffer) != 0:      # AT_FDCWD, STATX_BTIME
                return None
            if not struct.unpack_from('<I', buffer, 0)[0] & 0x800:
                return None
            seconds, nanoseconds = struct.unpack_from('<qI', buffer, 0x50)             # stx_btime
        except (OSError, AttributeError, TypeError, ValueError):
            return None
        return seconds+nanoseconds/1e9

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
                self.settle_dram_ownership()

    def settle_dram_ownership(self):
        if not ((self.record.get('npu_dirty') or self.record.get('dram_dirty')) and not self.record.get('dram_restored')):
            return
        if self.record.get('rom_returned') and self.record.get('dram_lost') and not self.armed and not self.failed:
            # A watchdog reset loses DRAM and returns a fresh ROM instance whose
            # disarm was read back; no saved DRAM can be restored or trusted.
            self.record['dram_released_by_reset'] = True
            self.journal()
        else:
            self.quarantine(DRAM_UNRESOLVED)

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

    def upload_credential(self, address, data):
        """Write the credential record and verify it by readback without any file: the source and the readback are
        anonymous memory files, so no byte of the record reaches the evidence directory, the queue or the disk."""
        base = images.load('board_sealed', ROOT/'tools/check-a733-smp.py')
        readback = os.memfd_create('wifi-readback', os.MFD_CLOEXEC)
        try:
            with base.sealed_source(bytes(data)) as (sealed, fd):
                self.sequence += 1
                self.tx('write', hex(address), sealed, 'read', hex(address), len(data), f'/proc/self/fd/{readback}',
                        timeout=max(30, len(data)*2/100000+10), pass_fds=(fd, readback))
            returned = os.pread(readback, len(data)+1, 0)
        finally:
            os.close(readback)
        if returned != bytes(data):
            raise Mismatch('Wi-Fi credential readback differs; execution refused.')

    def deliver_credential(self, plan):
        """Upload the credential as its own input, then zero the host copy. Only the name and fingerprint are recorded."""
        if self.credential is None:
            return
        region = plan.get('credential')
        require(region and region['bytes'] == len(self.credential), 'The session has no Wi-Fi credential region.')
        self.upload_credential(region['address'], self.credential)
        self.credential[:] = bytes(len(self.credential))
        self.credential = None
        self.record['wifi_delivered'] = True

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
        enumerated_at = None
        while True:
            try:
                node = self.access.discover(self.args.port)
                current = self.access.identity(node)
                if current != old:
                    enumerated_at = self.now() if enumerated_at is None else enumerated_at
                if current != old and os.access(node, os.R_OK | os.W_OK):
                    # Do not send a request before udev grants the new node.
                    self.adopt(node)
                    self.failed = False
                    self.deadline = None
                    return
            except (FileNotFoundError, PermissionError, ValueError):
                pass
            # A new instance has enumerated, so the board did return: only udev's permission grant is late, which a loaded
            # host delays. Give it a further 30 s instead of reporting no return.
            if self.now() >= deadline and not (enumerated_at is not None and self.now() < enumerated_at+30):
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
                    (self.record.get('dram_restored') or self.record.get('dram_unsaved')) and
                    not self.record.get('dram_lost'),
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
        # The pre-launch helper writes into the load region the image is about to
        # use, so its bytes are restored in every mode. The post-ROM helper runs
        # after the image is retired and nothing later reads that DRAM, so a
        # development run saves and restores nothing there.
        save = not post or images.preserving(self.args)
        backups = []
        if save:
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
        armed_at, armed_wall = self.elapsed_now(), self.wall_now()
        self.arm(0xb1,configuration=0x101)
        self.tx('reset64', '0x41000000')
        self.wait(26)
        before, after, words = self.returned()
        elapsed = self.elapsed_now()-armed_at
        # The host's elapsed time includes its own scheduling: under load a normal run measured 16.8 s against a usual 2 s,
        # and a refusal on it quarantined a healthy instance. The countdown ran on the board, so the decision rests on
        # what the board and the kernel report:
        #  * the helper returned the board to FEL, which enumerated a new instance whose device node the kernel created at
        #    that moment (instance_created); its distance from the host's stamp before the arm bounds the board's time from
        #    arming to return, from above (the countdown starts inside the arm transaction, after the stamp);
        #  * the WDT0 mode read on that instance still held the armed value and the disarm that followed read back clear;
        #  * the host talked to that same instance until the disarm, and a watchdog expiry in between would have removed
        #    its node, so the transactions would have failed (tx verifies the node's identity before every request).
        # A return later than the cutoff by the kernel's clock, or one the kernel cannot date, falls back to the host's
        # elapsed time.
        created = self.instance_created(self.node)
        enumerated = created-armed_wall if created is not None else None
        usable = enumerated is not None and 0 <= enumerated < 3600
        basis, deciding = ('kernel', enumerated) if usable else ('host', elapsed)
        self.record['rail_return'] = {'seconds':elapsed,'enumerated_seconds':enumerated,'decided_by':basis,
            'watchdog_configuration':0x101,
            'source_hz':26000000/750,'mode_before_disarm':before,'mode_after_disarm':after,'cutoff_seconds':14}
        run = {'return': self.record['rail_return'], 'evidence': str(evidence)}
        self.record.setdefault('rail_runs', {})[phase] = run
        if not (before&0xffff==0xb1 and not after&1 and 0<=deciding<14):
            self.record.update(dram_lost=True,setup_watchdog_reset=True)
            raise ValueError('Rail helper possible watchdog reset; no DRAM result read; rails were not normalized.')
        usb_power.enforce(self)
        data = self.read(0x42000000, 16384, evidence/'rail-result.bin')
        values = struct.unpack_from('<16Q', data)
        require(values[3] <= 16384-257, 'Invalid rail helper log length.')
        log = data[256:256+values[3]].decode(errors='replace')
        (evidence/'rail.log').write_text(log)
        # The helper performs all boot-clock, speed-bin and PMIC readbacks.
        refusal = None
        try:
            self.record['rails'] = images.post_rail_outcome(values, log) if post else images.rail_result(values, log)
        except images.RailRestoreRefused as error:
            # The helper's DRAM is still restored below; the refusal is raised afterwards, with its record.
            refusal = error
            self.record['rails'] = error.record
        run['image_sha256'] = expected
        run['rails'] = self.record['rails']
        if save:
            images.restore_dram(self, backups, regions)
            self.record.update(dram_restored=True, dram_dirty=False)
            run['dram_restored'] = True
        self.journal()
        if refusal:
            raise refusal

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
        if live.get('words'):
            self.record['live_words'] = dict(live['words'])
        if images.npu_on_required(info):
            live['words'].update(self.npu_on_state())
        plan = images.prepare(self.args, info, live, self.evidence/'bundle', dma_binding=self.dma_binding)
        plan['failure_return'] = bool(info.get('failure_return'))
        return plan

    def launch(self, plan):
        preserve = images.preserving(self.args)
        if not preserve and plan.get('format') in ('generic', 'npu'):
            # The pre-launch rail helper restored its own bytes; nothing else is saved or restored.
            self.record.update(dram_unsaved=True, dram_restored=False)
        if plan.get('format') == 'generic':
            images.validate_uploads(plan)
            if preserve:
                total, seconds = images.require_budget(plan, getattr(self.args, 'preserve_budget_s', None))
                self.record.update(preserve_bytes=total, preserve_estimate_s=round(seconds, 1))
                print('preservation: %d MiB backed up and restored, estimated %.0f s' % (total >> 20, seconds), flush=True)
                plan['dram_backups'] = images.backup_dram(self, plan['preserve_regions'])
                self.record['dram_backups'] = plan['dram_backups']
                self.record.update(dram_restored=False, dram_dirty=True)
                self.journal()
        if plan.get('format') == 'npu':
            plan['finite'].require_entry_state(self.tx)
            if preserve:
                backup = self.evidence/'npu-DRAM-before.bin'
                self.read(*plan['finite'].DRAM, backup)
                self.record['dram_restored'] = False
                self.record['npu_dirty'] = True
                plan.update(dram_backup=backup, dram_backup_sha256=images.sha(backup))
                self.record['dram_backup_sha256'] = plan['dram_backup_sha256']
                self.journal()
        for address, source in plan['uploads']:
            self.upload(address, source)
        self.deliver_credential(plan)
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
        if self.watchdog_reset_seen(before):
            self.record['dram_lost'] = True
            return 'watchdog-reset'
        # An image that links the failure return may return after its first
        # failure; the record is then in the breadcrumbs and in its result.
        # Any other return that carries a failure record stays unresolved.
        if not (decoded['session'] and decoded.get('last',{}).get('stage') == crumbs.RETURN_ROM and
                decoded['last']['value'] == 0 and not decoded.get('fault') and
                (not decoded.get('board_failure') or plan.get('failure_return'))):
            raise UnresolvedReturn('Return lacks the matching RETURN_ROM breadcrumb.')
        self.record['failure_return'] = bool(decoded.get('board_failure'))
        self.record['normal_return'] = True
        usb_power.enforce(self)
        return 'returned'

    def capture(self, plan):
        try:
            return images.capture(self, plan, self.args)
        except BaseException as error:
            if plan.get('format') in ('npu', 'generic'):
                if images.preserving(self.args):
                    if not self.record.get('dram_restored'):
                        self.quarantine('Native retirement or DRAM restoration unverified: '+str(error))
                elif plan.get('format') == 'npu' or not self.record.get('native_retirement_validated'):
                    # A development run promised no DRAM; only unproven retirement matters.
                    self.quarantine('Native retirement unverified: '+str(error))
            if isinstance(error, ValueError):
                raise Mismatch(str(error)) from error
            raise


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


def run_reference(args):
    """The reference a queued run carries; the submission already validated it."""
    value = getattr(args, 'reference', None)
    if not value:
        return None
    if value == 'none':
        return {'none': True, 'approved_by': getattr(args, 'approved_by', None),
                'reason': getattr(args, 'reference_reason', None)}
    path, _, anchor = value.partition('#')
    return {'path': path, 'anchor': anchor}


def run_image(args, backend=Live):
    started = time.monotonic()
    pressure_start = host_priority.pressure()
    evidence = args.evidence or CACHE/'board-queue/runs'/f'{time.time_ns()}-{args.port}-{uuid.uuid4().hex[:8]}'
    evidence = evidence_directory(evidence)
    result = {'schema': 'sunstone-board-result-v1', 'port': args.port, 'image': str(args.image),
              'kind': args.kind, 'outcome': 'host-error', 'first_failure': None, 'durations': {},
              'evidence_path': str(evidence.resolve()), 'result_path': str((evidence/'result.json').resolve()),
              'quarantined': False, 'launch_attempted': False, 'time': time.time(), 'host_pressure': {'start': pressure_start},
              'git_revision': subprocess.run(['git','-C',str(ROOT),'rev-parse','HEAD'], capture_output=True,text=True).stdout.strip()}
    result['dram_mode'] = 'preserved' if images.preserving(args) else 'development'
    result['reference'] = run_reference(args)
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
    credential = None
    try:
        if getattr(args, 'wifi', None):
            # Read the user's file here, in this process, before any other work: a missing file, a bad mode or a changed
            # fingerprint refuses the run before the device is touched. The result carries the name and fingerprint only.
            result['wifi'] = {'name': args.wifi, 'fingerprint': getattr(args, 'wifi_fingerprint', None)}
            entry = load_wifi(args.wifi, result['wifi']['fingerprint'])
            result['wifi']['fingerprint'] = wifi_module().fingerprint(entry['pmk'])
            credential = bytearray(wifi_module().board_record(entry))
            del entry
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
            if credential is not None:
                require(info.get('generic') and 'ss_wifi_credential' in images.symbols(frozen),
                        'The image does not receive a Wi-Fi credential (it does not link ss_wifi_credential); refused before device access.')
            dma_binding = images.dma_admission(args, info)
            application_inputs(args, info, evidence)
            images.load('storage_bounds', ROOT/'tools/storage_bounds.py').verify_delivered(args)
        owner = backend(args, evidence, result)
        owner.credential = credential
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
                            require(result.get('native_retirement_validated') and
                                    (result.get('dram_restored') or result.get('dram_unsaved')),
                                    'Rail restoration requires native retirement and restored DRAM.')
                        after = timed('restore-state', owner.live_state)
                        save(evidence/'state-after.json', after)
                        require(not state.boot_clocks(after['words']),
                                'CPU clocks differ from boot words before rail restoration.')
                        # Mandatory retirement and RAM restoration precede optional result validation.
                        # The application's result is valid whatever the helper says: record the helper's outcome
                        # beside it. A refusal that leaves the rails not shown to be at the target pauses the port
                        # (the daemon reads rails_unrestored); a board already at the target is a verified no-op.
                        try:
                            timed('restore-rails', lambda: owner.rails('restore-rails'))
                            result['rails_restored'] = True
                        except images.RailRestoreRefused as error:
                            result['rails_restored'] = False
                            result['rails_unrestored'] = True
                            result['first_failure_rails'] = str(error)
                        result['rail_restore'] = owner.record.get('rails')
                    if owner.record.get('failure_return'):
                        # A normal return that carries a failure record is a
                        # failed result. Retirement, DRAM and rails are already
                        # restored, so the port stays usable and unpaused.
                        origin = result['breadcrumbs']['board_failure']
                        result['first_failure'] = dict(origin, native_site=origin['site'],
                            site=f"{origin['group']}:{origin['site']}:{origin['caller']}",
                            detail=result['breadcrumbs'].get('summary', 'Failure return'),
                            record=captured['report'].get('failure'))
                        result['outcome'] = 'failure-return'
                        if owner.journal_error is not None:
                            raise owner.journal_error
                        return finish_result(result, evidence, started, stage)
                    if not captured['valid']:
                        # The image returned normally with a result record whose status or magic is not a success.
                        # Retirement, DRAM and rails are restored, so the port stays usable.
                        result['first_failure'] = {'site': 'capture', 'detail': 'The returned result record is not a success record.'}
                        result['outcome'] = 'failure-return'
                        return finish_result(result, evidence, started, stage)
                    if getattr(args, 'decoder', None):
                        decoder = load_decoder(args)
                        try:
                            decoded = decoder.decode(payload)
                        except (ValueError, struct.error) as error:
                            raise ValueError('Invalid decoded result: '+str(error)) from error
                        # The decoded result is the evidence; the reviewer compares it with the hypothesis.
                        result['decoded'] = decoded
                        if hasattr(decoder, 'failures'):
                            result['decoder_failures'] = decoder.failures(decoded)
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
        result['outcome'] = 'host-error' if result.get('launch_attempted') else 'refused-before-launch'
        result['first_failure'] = result.get('usb_vbus_fault', {'site': stage, 'detail': str(error)})
    except (ValueError, DramFailure) as error:
        result['outcome'] = 'host-error' if result.get('launch_attempted') else 'refused-before-launch'
        result['first_failure'] = {'site': stage, 'detail': str(error)}
    except BaseException as error:
        result['outcome'] = 'host-error'
        result['first_failure'] = {'site': stage, 'detail': f'{type(error).__name__}: {error}'}
        (evidence/'host-error.txt').write_text(traceback.format_exc())
    finally:
        if credential is not None:
            credential[:] = bytes(len(credential))
    return finish_result(result, evidence, started, stage)


def finish_result(result, evidence, started, stage):
    result['stage'] = stage
    result['durations']['total'] = round(time.monotonic()-started,6)
    # The kernel's I/O, CPU and memory stall figures at both ends of the run: a slow host stage is then attributed from data
    # (the stall time of the run is the difference of the two totals, in microseconds).
    result.setdefault('host_pressure', {})['end'] = host_priority.pressure()
    # Raw transaction evidence has its own record; result.json stays compact.
    summary = {k:v for k,v in result.items() if k != 'transactions'}
    save(evidence/'result.json', summary)
    print(f"{summary['port']} {summary['kind']} {summary.get('image_sha256','unreadable')[:12]} {summary['outcome']} {evidence/'result.json'}", flush=True)
    return summary


def result_summary(result, limit=1500):
    """The lines a waiting `submit --wait` prints: outcome, stage, first failure and the decoded application summary."""
    def short(value):
        text = json.dumps(value, sort_keys=True)
        return text if len(text) <= limit else text[:limit] + f'... ({len(text)} bytes of JSON in the result)'
    lines = [f"outcome: {result.get('outcome')}", f"stage: {result.get('stage')}",
             'first failure: ' + (short(result['first_failure']) if result.get('first_failure') else 'none')]
    if 'decoded' in result:
        decoded = result['decoded']
        if isinstance(decoded, dict) and 'passed' in decoded:
            lines.append(f"passed: {decoded['passed']}")
        lines.append('decoded: ' + short(decoded))
    else:
        lines.append('decoded: none (no decoder, or the image did not return a result)')
    if result.get('decoder_failures'):
        lines.append('decoder failures: ' + short(result['decoder_failures']))
    return lines


SESSION_LITERAL = re.compile(r"""['"]([A-Za-z0-9_./-]+\.(?:py|c|h|S|inc|json))['"]""")
SESSION_IMPORT = re.compile(r'^\s*(?:import|from)\s+([A-Za-z_]\w*)', re.M)
SESSION_INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


def session_files():
    """The repository files that prepare or admit a session: what tools/board.py and tools/board_images.py import, load and run.

    Derived, not listed: the closure over the modules they import and the repository paths their sources name, followed
    through included headers."""
    seen, stack = set(), ['tools/board.py', 'tools/board_images.py', 'tools/session.py']
    while stack:
        name = stack.pop()
        path = ROOT/name
        if name in seen or not path.is_file():
            continue
        seen.add(name)
        text = path.read_text(errors='replace')
        candidates = []
        if path.suffix == '.py':
            candidates += [os.path.normpath(m) for m in SESSION_LITERAL.findall(text) if not m.startswith('/')]
            candidates += ['tools/%s.py' % m for m in SESSION_IMPORT.findall(text)]
        elif path.suffix in ('.c', '.h', '.S', '.inc'):
            candidates += ['include/'+m for m in SESSION_INCLUDE.findall(text)]
            candidates += [os.path.relpath(os.path.normpath(path.parent/m), ROOT) for m in SESSION_INCLUDE.findall(text)]
        stack += [c for c in candidates if (ROOT/c).is_file()]
    return sorted(seen)


def require_daemon_code(queue_root):
    """Refuse a pre-flight that ran code the daemon will not run.

    serve() records its checkout in daemon.json. The runner the daemon starts prepares and admits the session with that
    checkout's files, so a check or submission from another checkout is valid only if every session file is the same."""
    record = Path(queue_root)/'daemon.json'
    if not record.is_file():
        return
    daemon = Path(json.loads(record.read_text())['root'])
    if daemon.resolve() == ROOT.resolve():
        return
    revision = subprocess.run(['git', '-C', str(daemon), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    differing = []
    for name in session_files():
        shown = subprocess.run(['git', '-C', str(daemon), 'show', revision+':'+name], capture_output=True)
        if shown.returncode or shown.stdout != (ROOT/name).read_bytes():
            differing.append(name)
    require(not differing, 'the daemon runs %s; these session files differ: %s; land them and restart the daemon first'
            % (revision[:12], ', '.join(differing)))


def check_image(args):
    """Apply launch admission and prepare uploads without constructing a USB owner."""
    require_daemon_code(getattr(args, 'queue', CACHE/'board-queue'))
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
    images.load('storage_bounds', ROOT/'tools/storage_bounds.py').verify_delivered(args)
    live = json.loads(args.state.read_text()) if args.state else {}
    dma_binding = images.dma_admission(args, info, live if live.get('identity') is not None else None)
    require(info.get('native_probe') or live.get('words'),
            'Offline application preparation requires --state with captured words.')
    plan = images.prepare(args, info, live, evidence/'bundle', dma_binding=dma_binding)
    report = {'admitted': True, 'physical_access': False, 'image_sha256': images.sha(args.image),
              'watchdog': plan['watchdog'],
              'uploads': [{'address': a, 'file': str(p), 'sha256': images.sha(p)} for a, p in plan['uploads']]}
    report.update({k: plan[k] for k in ('epoch', 'generation', 'lease', 'marker', 'host_admission') if k in plan})
    report['preflight'] = check_preflight(args, info, plan)
    save(evidence/'admission.json', report)
    print(json.dumps(report, indent=2))
    return 0


def apply_override(args, record):
    """Refuse a record whose pre-flight is unavailable unless --no-preflight REASON records an override."""
    reason = getattr(args, 'no_preflight', None)
    if reason is not None and not reason.strip():
        raise PreflightUnavailable('--no-preflight needs a reason.')
    if record.get('unavailable'):
        if reason is None:
            raise PreflightUnavailable('Pre-flight cannot run: %s. Fix that, or record an override with --no-preflight REASON.'
                                       % record['unavailable'])
        record = dict(record, override=reason.strip())
    return record


class PreflightUnavailable(ValueError):
    """The host pre-flight cannot run for an image that has a typed session; nothing is queued."""


NOT_TYPED_KINDS = ('probe', 'usb-class', 'rail-helper')


def check_preflight(args, info, plan):
    """The record of the host admission `check` ran, or the refusal when a typed session has none (see preflight_submission)."""
    if not info.get('generic') or info.get('finite_npu') or info.get('native_probe'):
        return {'admitted': None, 'preflight': 'not applicable', 'reason': 'not a typed-session OS image'}
    return apply_override(args, plan.get('host_admission') or {'admitted': None, 'unavailable': 'no host admission in the plan'})


def preflight_submission(args, source, directory):
    """Run the image's own EL3 admission on the session the runner will send, before the submission is queued.

    Builds the session as the runner does (from the frozen inputs and template, with the recorded board words in place of
    a live read) and runs the admission code compiled with the image's definitions (tools/host_admission.py). A refusal
    names the failing term and refuses the submission. An image that is not a typed-session OS image (a probe, a finite
    NPU image) has no host admission: its record says `not applicable`. When the pre-flight cannot run for an image that
    has a typed session, PreflightUnavailable refuses the submission unless --no-preflight REASON is given; the record
    then carries the reason and the override. `directory` is the queue entry; None builds the session from the arguments."""
    return apply_override(args, preflight_record(args, source, directory))


def preflight_record(args, source, directory):
    descriptor = args.descriptor or Path(str(source)+'.board.json')
    try:
        info = images.inspect(source, args.kind, json.loads(descriptor.read_text()) if descriptor.exists() else None,
                              args.host_tree)
    except (ValueError, OSError, KeyError) as error:
        if args.kind in NOT_TYPED_KINDS:
            return {'admitted': None, 'preflight': 'not applicable', 'reason': 'image not inspectable: %s' % error}
        return {'admitted': None, 'unavailable': 'image not inspectable: %s' % error}
    if not info.get('generic') or info.get('finite_npu') or info.get('native_probe'):
        return {'admitted': None, 'preflight': 'not applicable', 'reason': 'not a typed-session OS image'}
    words = images.offline_words()
    if words is None:
        return {'admitted': None, 'unavailable': 'no recorded board words on this machine (board_images.offline_words)'}
    frozen = argparse.Namespace(**vars(args))
    frozen.image = source
    if directory is not None:
        frozen.input = [directory/f'input-{i}.bin' for i, _ in enumerate(args.input)]
        frozen.template = directory/'template' if (directory/'template').is_dir() else args.template
    import tempfile
    with tempfile.TemporaryDirectory(prefix='preflight-', dir=CACHE) as scratch:
        try:
            plan = images.prepare(frozen, info, {'words': words}, Path(scratch)/'bundle', dma_binding=None)
        except images.AdmissionRefusal:
            raise
        except (ValueError, OSError, KeyError) as error:
            return {'admitted': None, 'unavailable': 'session not built offline: %s' % error}
    return plan.get('host_admission') or {'admitted': None, 'unavailable': 'no host admission in the plan'}


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
    parser.add_argument('--no-preflight', metavar='REASON')
    parser.add_argument('--approved-by', choices=('coordinator',))
    parser.add_argument('--host-tree', type=Path)
    parser.add_argument('--objcopy', type=Path)
    parser.add_argument('--port', default=os.environ.get('SUNSTONE_USB_PORT'), choices=PORTS)
    parser.add_argument('--evidence', type=Path)
    parser.add_argument('--state', type=Path)
    parser.add_argument('--check-only', action='store_true')
    parser.add_argument('--force', action='store_true')
    parser.add_argument('--normalize-rails', action=argparse.BooleanOptionalAction)
    parser.add_argument('--restore-rails', action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument('--preserve-dram', action='store_true')
    parser.add_argument('--preserve-budget-s', type=float)
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
    for name in ('template', 'manifest', 'descriptor', 'host_tree', 'objcopy', 'approved_by', 'no_preflight'):
        if getattr(args, name):
            command += ['--'+name.replace('_','-'), str(getattr(args, name))]
    if args.restore_rails and args.kind not in ('npu', 'usb-class'):
        command += ['--restore-rails']
    if args.preserve_dram:
        command += ['--preserve-dram']
    if getattr(args, 'preserve_budget_s', None) is not None:
        command += ['--preserve-budget-s', repr(args.preserve_budget_s)]
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
            text = path.read_text() if path.exists() else None
            state = json.loads(text) if text else {'jobs': [], 'pauses': {}}
            state.setdefault('history', [])
            for job in [j for j in state['jobs'] if j['state']=='done']:
                self.archive(state, job)
            yield state
            # The daemon takes this lock several times a second; rewrite and fsync only on change.
            if text is None or json.dumps(state, indent=2, sort_keys=True)+'\n' != text:
                save(path, state)

    HISTORY = 20  # per port: the status listing and the DRAM and release rules read the most recent runs

    @staticmethod
    def summary(job):
        """The few fields of a finished job that the queue's own rules and the status listing read."""
        result = job['result']
        return {'id': job['id'], 'worker': job['worker'], 'port': job['port'], 'assigned_port': job.get('assigned_port'),
                'kind': job.get('kind'), 'image_sha256': job['image_sha256'],
                'submitted': job['submitted'], 'completed': job.get('completed'), 'directory': job['directory'],
                'result_path': job['result_path'], 'outcome': result.get('outcome'), 'stage': result.get('stage'),
                'site': (result.get('first_failure') or {}).get('site'),
                'instance': (result.get('instances') or [{}])[-1].get('identity')}

    def archive(self, state, job):
        """Move a finished job out of the live state. The full record stays in its entry directory."""
        save(Path(job['directory'])/'entry.json', job)
        state['jobs'].remove(job)
        history = state['history'] + [self.summary(job)]
        port = lambda j: j.get('assigned_port') or j['port']
        state['history'] = [j for i, j in enumerate(history)
                            if sum(port(k)==port(j) for k in history[i+1:]) < self.HISTORY]

    def job(self, identifier):
        """The full record of a live or finished job."""
        with self.locked() as state:
            live = next((dict(j) for j in state['jobs'] if j['id']==identifier), None)
        if live is not None:
            return live
        path = self.root/'entries'/identifier/'entry.json'
        return json.loads(path.read_text()) if path.exists() else None

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
                  'launch_attempted': False, 'durations': {}, 'evidence_path': job['directory'],
                  'reference': job.get('reference'), 'wifi': job.get('wifi')}
        save(job['result_path'], result)
        job.update(state='done', result=result, completed=time.time())
        self.event('result', job=job['id'], outcome='refused-before-launch', detail=detail)
        self.archive(state, job)

    def submit(self, args):
        require(re.fullmatch(r'[A-Za-z0-9_.-]{1,80}', args.worker), 'Invalid worker name.')
        require(bool(args.hypothesis.strip()), 'A nonempty hypothesis is required.')
        require_daemon_code(self.root)
        reference = check_reference(args)
        wifi = None
        if getattr(args, 'wifi', None):
            # Only the name and the fingerprint are recorded; the value stays in the user's config directory.
            wifi = {'name': args.wifi, 'fingerprint': wifi_module().fingerprint(load_wifi(args.wifi)['pmk'])}
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
        options = {'reference': 'none' if reference.get('none') else f"{reference['path']}#{reference['anchor']}"}
        if reference.get('none'):
            options['reference_reason'] = reference['reason']
        for name in ('kind','approved_by','storage_write','host_tree','fel','rail_helper','ddr_build','ddr_restore','training_dir','restore_rails','preserve_dram','preserve_budget_s','decode_application','objcopy','no_preflight'):
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
        if wifi:
            options['wifi'], options['wifi_fingerprint'] = wifi['name'], wifi['fingerprint']
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
        # The image's own admission, on the exact session and contract, before the board could be touched.
        try:
            preflight = preflight_submission(args, source, directory)
        except (images.AdmissionRefusal, PreflightUnavailable):
            # Nothing was queued: move the half-built entry directory out of the queue (never delete it).
            trash = CACHE/'trash'/f'refused-submit-{identifier}'
            trash.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(directory), str(trash))
            raise
        if preflight.get('override'):
            print('pre-flight not run (override recorded): %s; reason: %s' % (preflight['unavailable'], preflight['override']),
                  file=sys.stderr)
        # Capture the complete queued input inventory, including templates.
        frozen = {str(p.relative_to(directory)): images.sha(p) for p in directory.rglob('*') if p.is_file()}
        job = {'id': identifier, 'submitted': time.time(), 'image': str(image),
               'image_sha256': hashlib.sha256(data).hexdigest(), 'port': args.port, 'kind': args.kind,
               'worker': args.worker, 'hypothesis': args.hypothesis.strip(), 'state': 'queued', 'reference': reference,
               'directory': str(directory), 'result_path': str(directory/'run/result.json'), 'options': options, 'frozen': frozen,
               'preflight': preflight}
        if wifi:
            job['wifi'] = wifi
        with self.locked() as state:
            state['jobs'].append(job)
            save(directory/'entry.json', job)
            self.event('entry', job=identifier, worker=job['worker'], port=job['port'], kind=job['kind'],
                       image_sha256=job['image_sha256'])
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
            if job.get('wifi'):
                try:
                    load_wifi(job['wifi']['name'], job['wifi']['fingerprint'])
                except ValueError as error:
                    self.refused(state, job, str(error))
                    return None
            job.update(state='running', assigned_port=port, started=time.time())
            self.event('running', job=job['id'], port=port)
            return dict(job)

    def complete(self, identifier, result):
        with self.locked() as state:
            job = next((j for j in state['jobs'] if j['id']==identifier), None)
            if job is None or job['state']=='done':
                return
            result['preflight'] = job.get('preflight')
            save(job['result_path'], result)
            images.record_board_words(result)
            job.update(state='done', result=result, completed=time.time())
            self.event('result', job=job['id'], outcome=result['outcome'], stage=result.get('stage'),
                       site=(result.get('first_failure') or {}).get('site'))
            self.archive(state, job)
            port = job['assigned_port']
            if result['outcome']=='no-return' or result.get('quarantined'):
                self.pause(state,port,'No return within the watchdog recovery window; inspect recovery before resuming.')
            history = [j for j in state['history'] if j.get('assigned_port')==port]
            if len(history)>=2 and all(j['stage']=='dram' and j['outcome']!='returned' for j in history[-2:]):
                self.pause(state,port,'Two consecutive DRAM bring-ups failed.')
            if result.get('rails_unrestored'):
                self.pause(state,port,'The CPU rails were not restored and are not shown to be at 800 mV; check them before resuming.')

    def resume(self, port):
        with self.locked() as state:
            state['pauses'].pop(port,None)
            path = self.root/f'paused-{port}.json'
            if path.exists():
                archive = self.root/'resumed'
                archive.mkdir(exist_ok=True)
                path.rename(archive/f'{time.time_ns()}-{port}.json')
            self.event('resumed',port=port)

    @staticmethod
    def refused_before_launch_is_safe(result, access, node):
        """True when a run refused before launch left DRAM ownership with nobody, and the instance is a live FEL one.

        Why the quarantine is safe to clear in this case: no image was launched, so nothing the run promised or the
        image owned lives in DRAM. The only DRAM the run wrote is the rail helper's two runner-owned regions (its load
        region and its 16 KiB result page), with their host-side backups; the helper's writes are residue that the next
        run's DRAM stage overwrites when it initializes DRAM from the training record before any later stage depends on
        it. The watchdog is the one hazard that outlives the run: it must be disarmed on this instance (the helper's
        return record shows the disarm read back clear, and the run then reset to a fresh instance, `fresh_fel`), the
        current instance's ROM descriptor must be the verified A733 service, and no transport or cleanup failure may
        have been recorded. A run that launched an image, or whose disarm or FEL state is unverified, still needs a
        power cycle."""
        if result.get('outcome')!='refused-before-launch' or result.get('launch_attempted') is not False:
            return False
        returned = ((result.get('rail_runs') or {}).get('rails') or {}).get('return') or result.get('rail_return') or {}
        if not (result.get('fresh_fel') is True and result.get('setup_watchdog_reset') is True and
                returned.get('mode_before_disarm',0)&0xffff==0xb1 and returned.get('mode_after_disarm',1)&1==0):
            return False
        try:
            return (access.target(node)/'bcdDevice').read_text().strip()=='02b3'
        except (OSError,ValueError):
            return False

    def release(self, port, reason, access=None):
        """Coordinator only: clear the DRAM-ownership quarantine of a fresh instance after a watchdog reset.

        Uses sysfs metadata and the device node's inode identity; the device is never opened."""
        require(port in PORTS,'Invalid port.')
        require(bool(reason.strip()),'A nonempty reason is required.')
        access = access or images.load('board_access', ROOT/'tools/usb-access.py')
        node = access.discover(port)
        observed = access.identity(node)
        require(access.target(node).name==port,'The enumerated instance is not on the requested port.')
        base = access.RECOVERY_REQUIRED
        paths = ([base] if base.exists() else []) + sorted(base.parent.glob(base.stem+'-*.json'))
        found = [(p,json.loads(p.read_text())) for p in paths]
        found = [(p,r) for p,r in found if r['identity']==observed]
        require(found,f'The instance enumerated on {port} has no quarantine record.')
        require(all(r['reason']==DRAM_UNRESOLVED for _,r in found),
                'Only an unresolved-DRAM-ownership quarantine can be released; other reasons need a replug.')
        with self.locked() as state:
            jobs = [j for j in state['history'] if j.get('assigned_port')==port]
        run = next((j for j in reversed(jobs) if j['instance']==observed),None)
        require(run is not None,'No finished run on this port ended on the quarantined instance.')
        result = json.loads(Path(run['result_path']).read_text())
        reset = (result.get('outcome')=='watchdog-reset' and result.get('rom_returned') is True and result.get('dram_lost') is True)
        refused = self.refused_before_launch_is_safe(result, access, node)
        require(reset or refused,
                'The run did not end in a watchdog reset with a verified ROM return, nor was it refused before any launch with '
                'its watchdog disarmed on a verified FEL instance; a replug is required.')
        require(not any(result.get(key) for key in ('cleanup_failure','usb_vbus_cleanup_failure','transport_recovered')),
                'The run recorded a cleanup or transport failure; a replug is required.')
        trash = CACHE/'trash/recovery-required-released'
        trash.mkdir(parents=True,exist_ok=True)
        for path,_ in found:
            shutil.move(str(path),str(trash/f'{time.time_ns()}-{path.name}'))
        self.event('released',port=port,job=run['id'],identity=observed,reason=reason.strip())
        return run['id']

    def withdraw(self, identifier, reason):
        """Refuse one queued entry before launch; running and finished entries are never touched."""
        require(re.fullmatch(r'[0-9]+-[a-f0-9]{8}',identifier),'Invalid entry ID.')
        require(bool(reason.strip()),'A nonempty reason is required.')
        with self.locked() as state:
            job = next((j for j in state['jobs'] if j['id']==identifier),None)
            if job is None and any(j['id']==identifier for j in state['history']):
                job = {'state': 'done'}
            require(job is not None,f'No queue entry {identifier}.')
            require(job['state']=='queued',f"Entry {identifier} is {job['state']}; only a queued entry can be withdrawn.")
            self.refused(state,job,'Withdrawn by the coordinator: '+reason.strip())
            self.event('withdrawn',job=identifier,reason=reason.strip(),port=job['port'])

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
                'results':[j for j in state['history'] if (j.get('assigned_port') or j['port'])==port][-20:]}
                for port in PORTS}}

    @staticmethod
    def runner_command(job, port):
        """The runner process for a claimed job. A Wi-Fi credential appears as its name and fingerprint only."""
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
        return command

    def serve(self, once=False):
        with (self.root/'daemon.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX|fcntl.LOCK_NB)
            # The checkout whose files every runner this daemon starts will use (require_daemon_code).
            save(self.root/'daemon.json', {'root': str(ROOT), 'pid': os.getpid(), 'started_revision': subprocess.run(
                ['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()})
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
                            command = self.runner_command(job,port)
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
    parser.add_argument('--manifest', type=Path, help='Exact finite NPU build manifest.')
    parser.add_argument('--objcopy', type=Path, default=Path('/usr/bin/llvm-objcopy-21'))
    parser.add_argument('--approved-by', choices=('coordinator',))
    parser.add_argument('--reference', help='PATH#ANCHOR of the tracked docs heading that derives the subsystem vendor sequence and the diff against it; '
                        '"none" needs --approved-by coordinator and --reference-reason.')
    parser.add_argument('--reference-reason', help='Why a coordinator exempts this run from a reference.')
    parser.add_argument('--storage-write', action='store_true')
    parser.add_argument('--no-preflight', metavar='REASON',
                        help='Queue or check an image whose host pre-flight cannot run; REASON is recorded in the entry and the result.')
    parser.add_argument('--descriptor', type=Path, help='Hash-bound image metadata; defaults to IMAGE.board.json.')
    parser.add_argument('--restore-rails', action='store_true', help='Restore guarded CPU rails after a validated normal return.')
    parser.add_argument('--preserve-dram', action='store_true',
                        help='Qualification run: save the image DRAM before launch and restore it after retirement. Development runs skip both.')
    parser.add_argument('--preserve-budget-s', type=float,
                        help='Longest estimated backup and restore time, in seconds, that a --preserve-dram run accepts; the plan is refused above it.')
    parser.add_argument('--decoder', type=Path, help='Application payload decoder; its decoded result is recorded in the result.')
    parser.add_argument('--decoder-origin', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('--decode-application', action='store_true', help='Use the application decoder and its default inputs when available.')
    parser.add_argument('--records', type=Path, help='Board, thermal and CPU session records for a generic image.')
    parser.add_argument('--template', type=Path, help='Typed-session bundle, or legacy bundle with board/ and assets/.')
    parser.add_argument('--host-tree', type=Path, help='Host source tree for image geometry, session preparation and return validation.')
    parser.add_argument('--input', action='append', type=Path, default=[])
    parser.add_argument('--wifi', metavar='NAME', help='Deliver the Wi-Fi credential NAME (created by tools/wifi-credential.py) to the board as a separate input. '
                        'The job records the name and the credential fingerprint; the daemon reads the file at launch.')
    parser.add_argument('--wifi-fingerprint', help=argparse.SUPPRESS)
    parser.add_argument('--fel', type=Path, default=state.FEL)
    parser.add_argument('--rail-helper', type=Path, default=state.RAIL_HELPER)
    parser.add_argument('--ddr-build', type=Path, default=CACHE/'build/a733-ddr')
    parser.add_argument('--ddr-restore', type=Path, default=CACHE/'build/a733-ddr-restore')
    parser.add_argument('--training-dir', type=Path, default=CACHE/'ddr-records')
    parser.add_argument('--process-record',type=Path,help=argparse.SUPPRESS)


def main(argv=None):
    host_priority.raise_runner()      # the runner and the daemon keep their I/O ahead of the gate's (tools/host_priority.py)
    host_space.use_disk_tmp(CACHE)    # temporary files on the main disk: /tmp has run out of inodes and of quota (tools/host_space.py)
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
    waiting = submit.add_mutually_exclusive_group()
    waiting.add_argument('--no-wait',action='store_true',help='Print the result path without waiting for the result.')
    waiting.add_argument('--wait',action='store_true',
                         help='Wait for the result, then print its outcome, stage, first failure and decoded summary before the path; '
                              'exit 0 only for a returned image.')
    commands.add_parser('status')
    watch = commands.add_parser('watch')
    watch.add_argument('entry',nargs='?',help='Entry ID to wait for, or display status on changes.')
    serve = commands.add_parser('serve')
    serve.add_argument('--once',action='store_true',help='Drain currently runnable entries and exit.')
    resume = commands.add_parser('resume')
    resume.add_argument('port',choices=PORTS)
    withdraw = commands.add_parser('withdraw',help='Coordinator only: refuse one queued entry before launch.')
    withdraw.add_argument('entry')
    withdraw.add_argument('--reason',required=True)
    release = commands.add_parser('release',help='Coordinator only: clear the quarantine of a fresh instance after a watchdog reset.')
    release.add_argument('port',choices=PORTS)
    release.add_argument('--reason',required=True)
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
            try:
                while not path.exists():
                    time.sleep(.2)
            except KeyboardInterrupt:
                print(f"still queued: {job['id']} (board.py watch {job['id']}); result will be {path}",file=sys.stderr)
                return 130
        if args.wait:
            result = json.loads(path.read_text())
            print('\n'.join(result_summary(result)))
            print(path)
            return 0 if result.get('outcome')=='returned' and not result.get('decoder_failures') else 1
        print(path)
    elif args.command=='status':
        print(json.dumps(queue.status(),indent=2))
    elif args.command=='resume':
        queue.resume(args.port)
        print(f'{args.port} resumed')
    elif args.command=='release':
        print(f"{args.port} released (run {queue.release(args.port,args.reason)}); resume the port if it is paused")
    elif args.command=='withdraw':
        queue.withdraw(args.entry,args.reason)
        print(f'{args.entry} withdrawn')
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
