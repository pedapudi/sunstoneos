#!/usr/bin/env python3
"""Check MD5/SM3 descriptors, stream boundaries and native request parsing on the host."""
import argparse
import ast
import hashlib
import hmac
import importlib.util
import json
from pathlib import Path
import subprocess
from harness import CACHE, CLANG, SANITIZE, environment, refused

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--host-request', type=Path, required=True,
               help='Reviewed host producer with MD5/SM3 parser patch applied')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
a.output.mkdir(parents=True, exist_ok=False)
spec = importlib.util.spec_from_file_location('digest_request', a.host_request)
request = importlib.util.module_from_spec(spec)
spec.loader.exec_module(request)
assert request.HASHES['md5'] == 0 and request.HASHES['sm3'] == 6

# Authenticate the cached source bytes before using their selectors/padding.
manifest = json.loads((root / 'apps/crypto/security-sources.json').read_text())
sources = {}
for name, row in manifest['pins'].items():
    path = CACHE / 'sources' / row['archive']
    assert hashlib.sha256(path.read_bytes()).hexdigest() == row['sha256'], name
    if path.suffix in ('.h', '.c', '.txt'):
        sources[name] = path.read_text()
reg = sources['ce-v5-registers']
assert '#define SS_METHOD_MD5\t\t\t0' in reg and '#define SS_METHOD_SM3\t\t\t6' in reg
assert 'int big_endian = type == SS_METHOD_MD5 ? 0 : 1;' in sources['ce-common']

# Reuse SHA tables only. New MD5 and SM3 compression code is in the model;
# complete answers below come from hashlib/OpenSSL, independently of that code.
tree = ast.parse((root / 'checks/a733-ce-modes.py').read_text())
names = {'primes', 'k512', 'sha384', 'initial'}
nodes = [n for n in tree.body if
         (isinstance(n, ast.FunctionDef) and n.name == 'root') or
         (isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id in names for t in n.targets))]
tables = {}
exec(compile(ast.Module(body=nodes, type_ignores=[]), 'sha-model-tables', 'exec'), tables)
tables['initial'][0] = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0, 0, 0, 0]
tables['initial'][6] = [0x7380166f, 0x4914b2b9, 0x172442d7, 0xda8a0600,
                        0xa96f30bc, 0x163138aa, 0xe38dee4d, 0xb0fb0e4e]
table_text = 'k512 ' + ' '.join('%x' % v for v in tables['k512']) + '\n'
table_text += ''.join('initial %u ' % m + ' '.join('%x' % v for v in values) + '\n'
                      for m, values in tables['initial'].items())
table_file = a.output / 'tables.txt'
table_file.write_text(table_text)
commands = []
def run(command, label):
    command = list(map(str, command))
    commands.append(command)
    result = subprocess.run(command, cwd=root, env=environment(), text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (a.output / (label + '.log')).write_text(result.stdout)
    (a.output / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    if result.returncode:
        raise SystemExit(result.stdout or result.returncode)
    return result.stdout

common = ['drivers/crypto/a733_ce.c', 'drivers/crypto/a733_ce_session.c',
          'drivers/crypto/a733_ce_hash.c', 'drivers/crypto/a733_ce_cipher.c',
          'drivers/clock/a733_clock_leases.c', 'runtime/device_memory_pool.c']
objects = []
for i, source in enumerate(common):
    obj = a.output / ('common-%u.o' % i)
    run([CLANG, '-std=c11', '-O1', '-g', *SANITIZE, '-ffunction-sections',
         '-fdata-sections', '-Iinclude', '-c', source, '-o', obj], 'compile-%u' % i)
    objects.append(obj)
for name, extra in [('service', ['checks/crypto-digests.c']),
                    ('application', ['checks/crypto-modes.c', 'apps/crypto/run.c',
                                     'apps/crypto/request.c', 'apps/crypto/vectors.c',
                                     'drivers/storage/storage_digest.c'])]:
    run([CLANG, '-std=c11', '-O1', '-g', *SANITIZE, '-ffunction-sections',
         '-fdata-sections', '-Wl,--gc-sections', '-Iinclude', '-Iapps/crypto',
         '-Ichecks', *objects, *extra, '-o', a.output / name], 'link-' + name)

lengths = (0, 1, 55, 56, 63, 64, 65, 119, 120, 127, 128, 2047, 2048, 2049, 4097)
service_text = table_text
expected_lines = []
for method, selector in [('md5', 0), ('sm3', 6)]:
    for size in lengths:
        message = bytes((i * 37 + 11) & 255 for i in range(size))
        for key_size in (None, 0, 16, 64, 65, 128, 1024):
            key = None if key_size is None else bytes(i & 255 for i in range(key_size))
            encoded = '*' if key is None else key.hex() or '-'
            # The existing model uses a dash as an empty hex key.
            service_text += 'hash %u %s %u\n' % (selector, encoded, size)
            expected = hashlib.new(method, message).digest() if key is None else hmac.new(key, message, method).digest()
            expected_lines.append((selector, encoded, size, expected.hex()))
service_file = a.output / 'service-input.txt'
service_file.write_text(service_text)
output = run([a.output / 'service', service_file], 'service')
observed = [line.split() for line in output.splitlines() if line.startswith('hash ')]
assert len(observed) == len(expected_lines)
for line, (selector, key, size, digest) in zip(observed, expected_lines):
    assert line == ['hash', str(selector), key, str(size), digest], (line, selector, size)

# Published empty/abc known answers catch agreement on the wrong algorithm.
kats = {'md5': ('d41d8cd98f00b204e9800998ecf8427e', '900150983cd24fb0d6963f7d28e17f72'),
        'sm3': ('1ab21d8355cfa17f8e61194831e81a8f22bec8c728fefb747ed035eb5082aa2b',
                '66c7f0f462eeedd9d1f2d46bdc10e4e24167c4875cf2f7a2297da02b8f4ba8e0')}
cases = []
for method in ('md5', 'sm3', 'sha256'):
    messages = [b'', b'abc', bytes(range(256)) * 8]
    for kind, key in [('hash', b''), ('hmac', b''), ('hmac', bytes(range(128)))]:
        for index, message in enumerate(messages):
            name = '%s-%s-%u-%u' % (method, kind, len(key), index)
            case = a.output / name
            case.mkdir()
            raw = request.encode_extended(kind, 1, message, method=method, key=key)
            expected = hashlib.new(method, message).digest() if kind == 'hash' else hmac.new(key, message, method).digest()
            if method in kats and kind == 'hash' and index < 2:
                assert expected.hex() == kats[method][index]
            (case / 'request.bin').write_bytes(raw)
            (case / 'expected.bin').write_bytes(expected)
            assert request.decode(raw)['output_bytes'] == len(expected)
            run([a.output / 'application', table_file, case / 'request.bin',
                 case / 'expected.bin', case / 'output.bin', 'success'], name)
            assert (case / 'output.bin').read_bytes() == expected
            cases.append(name)

# Authenticated reserved selectors reach the native parser; an unrelated hash
# mismatch must not count as the selector refusal.
for selector in (7, 0xffffffff):
    import struct
    raw = bytearray(request.encode_extended('hash', 1, b'abc', method='sha256'))
    struct.pack_into('<I', raw, 2096, selector)
    refused(lambda: request.decode(raw), 'Invalid hash parameters')
    path = a.output / ('invalid-%u.bin' % selector)
    path.write_bytes(raw)
    run([a.output / 'application', table_file, path, path, path, 'parse-refused'], 'invalid-%u' % selector)

inputs = common + ['checks/a733-ce-modes.c', 'checks/crypto-modes.c',
                   'checks/crypto-digests.c', 'checks/crypto-digests.py', 'apps/crypto/run.c',
                   'apps/crypto/request.c', 'include/a733_ce_digest_methods.h']
(a.output / 'results.json').write_text(json.dumps({
    'result': 'PASS', 'physical_access': False, 'service_cases': len(expected_lines),
    'application_cases': cases, 'host_request_sha256': hashlib.sha256(a.host_request.read_bytes()).hexdigest(),
    'source_inputs': {s: hashlib.sha256((root / s).read_bytes()).hexdigest() for s in inputs}}, indent=2) + '\n')
print('PASS MD5/SM3 stream, padding, HMAC, KAT and application parser checks; no hardware')
