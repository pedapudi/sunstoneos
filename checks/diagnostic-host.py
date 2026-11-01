"""Exercise diagnostic admission and safety failures with an in-memory transport."""
from contextlib import contextmanager
import harness
import hashlib
import importlib.util
import json
from pathlib import Path
import runpy
import struct
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
CASES = []


def exercise(name, fault=None):
    with tempfile.TemporaryDirectory(prefix='diagnostic-host-') as directory:
        root = Path(directory)
        evidence = root / 'evidence'
        evidence.mkdir()
        (evidence / 'earlier.log').write_text('saved evidence')
        image = root / 'image.bin'
        image.write_bytes(bytes(0x801) if fault == 'oversized' else b'current image')
        rom = root / 'rom.bin'
        rom.write_bytes(bytes(0x11000))
        fel = root / 'fel'
        fel.write_bytes(b'current tool')
        port = root / '3-1'
        port.mkdir()
        (port / 'product').write_text('FEL')
        nodes = [Path('/virtual/003/001'), Path('/virtual/003/002'), Path('/virtual/003/003')]
        memory = {}
        registers = [0xa2, 0x22, 7, 0, 0x12340000, 0, 0x76540000, 0x10203040]
        state = dict(node=nodes[0], calls=[], quarantined=[], locked=False, executions=0, held=False)

        def write(address, data):
            memory.update((address + index, value) for index, value in enumerate(data))

        def read(address, size):
            return bytes(memory.get(address + index, 0) for index in range(size))

        @contextmanager
        def lock():
            assert not state['locked']
            state['locked'] = True
            try: yield nodes[0]
            finally: state['locked'] = False

        @contextmanager
        def reenumeration(*args):
            yield {'count': args[-1]}

        access = SimpleNamespace(board_lock=lock, require_session=lambda n: None,
            require_rom=lambda n: None, identity=lambda n: [str(n)], target=lambda n: port,
            session_node=lambda config: state['node'],
            quarantine_instance=lambda *args: state['quarantined'].append(args))
        guard = SimpleNamespace(check_health=lambda: None, reenumeration=reenumeration)

        def command(argv, **kwargs):
            assert state['locked']
            args = list(argv[3:])
            state['calls'].append(args.copy())
            if fault == 'dram-interrupt' and len(state['calls']) == 5:
                raise KeyboardInterrupt('interrupted restoration')
            if fault == 'transport' and len(state['calls']) == 2 or fault == 'dram-transport' and len(state['calls']) == 5:
                return SimpleNamespace(returncode=1, stdout='', stderr='transport stopped')
            output = []
            while args:
                op = args.pop(0)
                if op == 'version': output.append('wrong SoC' if fault == 'target' else 'soc=00001903(A733)')
                elif op == 'read':
                    address, size, path = args[:3]; del args[:3]
                    data = read(int(address, 0), int(size))
                    if fault == 'upload' and str(path).endswith('uploaded.bin'): data = b'bad'
                    if fault == 'dram' and str(path).endswith('dram-tested.bin'): data = b'bad'
                    if fault == 'dram-restore' and str(path).endswith('dram-restored.bin'): data = b'bad'
                    Path(path).write_bytes(data)
                elif op == 'write':
                    address, path = args[:2]; del args[:2]
                    write(int(address, 0), Path(path).read_bytes())
                elif op == 'memmove':
                    destination, source, size = map(lambda v: int(v, 0), args[:3]); del args[:3]
                    write(destination, rom.read_bytes()[source:source+size])
                elif op == 'readl':
                    address = int(args.pop(0), 0)
                    addresses = (0x02050014, 0x02050010, 0x02050018, 0x070501c0,
                                 0x070501c4, 0x070501d4, 0x070501d8, 0x07090108)
                    value = registers[addresses.index(address)] if address != 0x03000024 else 0
                    output.append(hex(value))
                elif op == 'exe':
                    args.pop(0); state['executions'] += 1
                    values = (0x57444346, 0x57525031, 0, 3, *registers[:6],
                              0xfa50392f, 0x20, (registers[1] & ~3) | 1, 2, 0, registers[0])
                    write(0x52800, struct.pack('<16I', *values))
                elif op == 'reset64':
                    args.pop(0); state['executions'] += 1; state['node'] = nodes[1]
                    (port / 'product').write_text('sunstoneos RAM diagnostics')
                else: raise AssertionError(op)
            return SimpleNamespace(returncode=0, stdout=' '.join(output), stderr='')

        class Device:
            bcdDevice = 0x0100
            ticks = 0
            def set_configuration(self, value): assert value == 1
            def write(self, endpoint, packet, **kwargs):
                self.packet = packet
                return len(packet)
            def read(self, endpoint, size, **kwargs):
                magic, sequence, kind, length = struct.unpack_from('<4I', self.packet)
                payload = self.packet[16:]
                if kind == 1:
                    self.ticks += 3001
                    payload = struct.pack('<5Q', self.ticks, 4, 1, 2, 100)
                elif kind == 3: payload = b'bounded diagnostic log\n'
                elif kind == 4:
                    payload = b''; state['node'] = nodes[2]
                    (port / 'product').write_text('FEL')
                    write(0x42000000, struct.pack('<16Q', 0x534b4552, 5, *([0]*14)))
                return struct.pack('<4I', magic, sequence, kind | 0x80000000, len(payload)) + payload

        path = TOOLS / name
        argv = [str(path), '--fel', str(fel), '--image', str(image), '--evidence', str(evidence)]
        if name == 'check-fel-input.py': argv = [str(path), '--fel', str(fel), '--evidence', str(evidence)]
        if name != 'usb-diagnostic.py': argv += ['--rom', str(rom)]
        original_spec = importlib.util.spec_from_file_location
        def spec_for(name, path, *args, **kwargs):
            spec = original_spec(name, path, *args, **kwargs)
            if Path(path).name == 'check-a733-smp.py':
                loader = spec.loader
                def load_base(module):
                    loader.exec_module(module)
                    def hold():
                        assert state['locked'] and state['quarantined']
                        state['held'] = True
                    def stop(record, save, quarantine, error):
                        return module.stop_original(record, save, quarantine, error, hold=hold)
                    module.stop_original = module.stop_after_failure
                    module.stop_after_failure = stop
                spec.loader = SimpleNamespace(create_module=lambda spec: None, exec_module=load_base)
            if Path(path).name == 'usb-access.py':
                spec.loader = SimpleNamespace(create_module=lambda spec: access, exec_module=lambda module: None)
            return spec
        failure = None
        with patch.object(sys, 'argv', argv), patch.dict(sys.modules, {'usb_guard': guard}), \
             patch.object(importlib.util, 'spec_from_file_location', side_effect=spec_for), \
             patch('subprocess.run', side_effect=command):
            try:
                if name == 'usb-diagnostic.py':
                    core = SimpleNamespace(find=lambda **kwargs: Device(), USBError=RuntimeError)
                    util = SimpleNamespace(claim_interface=lambda *a: None, release_interface=lambda *a: None,
                                           dispose_resources=lambda *a: None)
                    with patch.dict(sys.modules, {'usb': SimpleNamespace(core=core, util=util), 'usb.core': core, 'usb.util': util}), \
                         patch('os.access', return_value=True), patch('time.sleep'):
                        runpy.run_path(str(path), run_name='__main__')
                else:
                    spec = original_spec('diagnostic_under_test', path)
                    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
                    module.ROM_HASH = hashlib.sha256(rom.read_bytes()).hexdigest()
                    status = module.main()
                    if status: failure = status
            except SystemExit as error:
                failure = error.code
            except KeyboardInterrupt:
                assert fault == 'dram-interrupt'
                failure = 'interrupted restoration'
        assert bool(failure) == bool(fault), (name, fault, failure)
        assert (evidence / 'earlier.log').read_text() == 'saved evidence'
        if fault == 'oversized':
            assert not state['calls']
        else:
            sessions = list(evidence.glob('*/session.json'))
            assert len(sessions) == 1
            record = json.loads(sessions[0].read_text())
            assert record['complete'] is (fault is None)
            if fault: assert state['executions'] == 0
            if fault == 'transport': assert len(state['calls']) == 2
            if fault in ('dram-transport','dram-restore','dram-interrupt'):
                assert record['dram_probe_dirty'] and state['held']
                assert len(state['calls']) == 5
            elif name == 'usb-diagnostic.py': assert not state['held']
        CASES.append(dict(tool=name, fault=fault, requests=len(state['calls'])))


for name in ('check-watchdog-configuration.py', 'check-fel-input.py', 'usb-diagnostic.py'):
    for fault in (None, 'target', 'transport'):
        exercise(name, fault)
    if name != 'check-fel-input.py': exercise(name, 'upload')
    if name == 'check-watchdog-configuration.py': exercise(name, 'oversized')
    if name == 'usb-diagnostic.py':
        for fault in ('dram','dram-restore','dram-transport','dram-interrupt'): exercise(name, fault)
print(json.dumps(dict(passed=True, cases=CASES, physical_access=False), indent=2))
