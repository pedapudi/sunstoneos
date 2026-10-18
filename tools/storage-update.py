#!/usr/bin/env python3
"""Preserve card bytes and prepare bounded native updates under trusted host control.

Preparation does not launch the board. A write-capable session additionally needs
an independently qualified maintenance watchdog and reviewed native launch. The
host is trusted to preserve its fsynced files; hashes do not prove physical reset.
"""
import argparse
import contextlib
import fcntl
import hashlib
import functools
import importlib.util
import json
import os
from pathlib import Path
import secrets
import stat
import struct

CHUNK = 1024
ZERO = bytes(32)


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).digest()


def canonical(value):
    return (json.dumps(value, sort_keys=True, separators=(',', ':')) + '\n').encode()


def identity(info):
    return (info.st_dev, info.st_ino, info.st_mode, info.st_uid, info.st_gid,
            info.st_size, info.st_nlink, info.st_mtime_ns, info.st_ctime_ns)


def sync_directory(fd):
    os.fsync(fd)


class ProtectedDirectory:
    """Hold a private directory and exclusive advisory lock for the entire job."""
    def __init__(self, path, create=False):
        self.path = Path(path)
        if create:
            self.path.mkdir(mode=0o700, parents=False, exist_ok=False)
            parent = os.open(self.path.parent, os.O_DIRECTORY | os.O_RDONLY)
            try:
                os.fsync(parent)
            finally:
                os.close(parent)
        self.fd = os.open(self.path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        info = os.fstat(self.fd)
        require(info.st_uid == os.getuid() and stat.S_IMODE(info.st_mode) == 0o700, 'Artifact directory must be owned by this user with mode0700.')
        fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.files = []

    def close(self):
        for fd in self.files:
            os.close(fd)
        self.files.clear()
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def create(self, name):
        require('/' not in name and name not in ('', '.', '..'), 'Invalid artifact name.')
        fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=self.fd)
        self.files.append(fd)
        return fd

    def seal(self, name, fd, expected_size, expected_hash):
        os.fsync(fd)
        os.fchmod(fd, 0o400)
        os.fsync(fd)
        original = os.fstat(fd)
        sync_directory(self.fd)
        sealed = self.open(name, expected_size, expected_hash)
        observed = os.fstat(sealed)
        require((original.st_dev, original.st_ino) == (observed.st_dev, observed.st_ino), 'Sealed file identity changed.')
        os.close(fd)
        self.files.remove(fd)
        return sealed

    def open(self, name, size=None, sha=None):
        require('/' not in name and name not in ('', '.', '..'), 'Invalid artifact name.')
        fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=self.fd)
        self.files.append(fd)
        info = os.fstat(fd)
        require(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid() and stat.S_IMODE(info.st_mode) == 0o400 and info.st_nlink == 1,
                'Protected artifact must be a private sealed regular file.')
        require(size is None or info.st_size == size, 'Protected artifact size differs.')
        got = hash_fd(fd)
        require(sha is None or got == sha, 'Protected artifact hash differs.')
        require(identity(os.fstat(fd)) == identity(info), 'Protected artifact changed during verification.')
        return fd

    def record(self, name, value):
        data = canonical(value)
        fd = self.create(name)
        write_all(fd, data)
        return self.seal(name, fd, len(data), digest(data)), digest(data)

    def json(self, name):
        fd = self.open(name)
        require(os.fstat(fd).st_size <= 1 << 20, 'Metadata exceeds bounded size.')
        return json.loads(os.pread(fd, os.fstat(fd).st_size, 0))


def write_all(fd, data):
    remaining = memoryview(data)
    while remaining:
        count = os.write(fd, remaining)
        require(count > 0, 'Artifact write made no progress.')
        remaining = remaining[count:]


def hash_fd(fd):
    h = hashlib.sha256()
    offset = 0
    while True:
        data = os.pread(fd, 65536, offset)
        if not data:
            return h.digest()
        offset += len(data)
        h.update(data)


def snapshot(store, name, source, size):
    source_fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        before = os.fstat(source_fd)
        require(stat.S_ISREG(before.st_mode) and before.st_size == size, 'Candidate must fill the exact authorized extent.')
        target = store.create(name)
        sha = hashlib.sha256()
        offset = 0
        while offset < size:
            data = os.pread(source_fd, min(65536, size - offset), offset)
            require(data, 'Candidate was truncated.')
            write_all(target, data)
            sha.update(data)
            offset += len(data)
        require(identity(os.fstat(source_fd)) == identity(before), 'Candidate changed during snapshot.')
        result = sha.digest()
        return store.seal(name, target, size, result), result
    finally:
        os.close(source_fd)


def grant_encode(g):
    p = bytearray(256)
    struct.pack_into('<6I', p, 0, 0x53554731, 1, g['mode'], g['kind'], g['host'], 0)
    p[24:56] = bytes.fromhex(g['nonce'])
    struct.pack_into('<4I4Q', p, 56, *g['cid'], g['sectors'], g['first'], g['blocks'], g['deadline_us'])
    for offset, key in ((104, 'backup'), (136, 'candidate'), (168, 'allocation'), (200, 'recovery')):
        value = bytes.fromhex(g[key])
        require(len(value) == 32, 'Grant hash must be SHA256.')
        p[offset:offset + 32] = value
    require(len(p) == 256, 'Invalid grant nonce length.')
    return bytes(p)


def grant_decode(p):
    require(len(p) == 256 and struct.unpack_from('<2I', p) == (0x53554731, 1) and not any(p[232:]) and not any(p[20:24]), 'Invalid native grant.')
    mode, kind, host = struct.unpack_from('<3I', p, 8)
    cid = list(struct.unpack_from('<4I', p, 56))
    sectors, first, blocks, deadline = struct.unpack_from('<4Q', p, 72)
    require(mode in (0, 1) and kind in (1, 2) and host in (0, 1) and (kind != 1 or host == 0) and 0 < blocks <= sectors - first,
            'Invalid native range.')
    g = dict(mode=mode, kind=kind, host=host, nonce=p[24:56].hex(), cid=cid, sectors=sectors, first=first, blocks=blocks, deadline_us=deadline)
    for offset, key in ((104, 'backup'), (136, 'candidate'), (168, 'allocation'), (200, 'recovery')):
        g[key] = p[offset:offset + 32].hex()
    return g


def card_range(g):
    return {k: g[k] for k in ('kind', 'host', 'cid', 'sectors', 'first', 'blocks', 'allocation')}


def make_chain(store, fd, size):
    """Store next commitments backwards; each write verifies before card I/O."""
    count = (size + CHUNK - 1) // CHUNK
    table = store.create('chain.bin')
    os.ftruncate(table, count * 32)
    following = ZERO
    for index in range(count - 1, -1, -1):
        data = os.pread(fd, min(CHUNK, size - index * CHUNK), index * CHUNK)
        require(len(data) == min(CHUNK, size - index * CHUNK), 'Candidate snapshot is incomplete.')
        require(os.pwrite(table, following, index * 32) == 32, 'Commitment table write incomplete.')
        following = digest(struct.pack('<QII', index * 2, len(data), 0) + following + data)
    # Reopen only after sealing; hash the write-only descriptor through a fresh read descriptor.
    os.fsync(table)
    tmp = os.open('chain.bin', os.O_RDONLY | os.O_NOFOLLOW, dir_fd=store.fd)
    try:
        chain_hash = hash_fd(tmp)
    finally:
        os.close(tmp)
    return store.seal('chain.bin', table, count * 32, chain_hash), following, chain_hash


class Client:
    def __init__(self, exchange, grant):
        self.exchange = exchange
        self.grant = dict(grant)
        self.sequence = 0
        self.failed = False

    def call(self, command, offset=0, payload=b'', receipt=ZERO, following=None):
        require(not self.failed, 'An uncertain session cannot issue further requests.')
        self.sequence += 1
        packet = bytes.fromhex(self.grant['nonce']) + struct.pack('<IIQII', self.sequence, command, offset, len(payload), 0) + receipt
        if following is not None:
            packet += following
        packet += payload
        try:
            response = self.exchange(packet)
            require(len(response) >= 56, 'Incomplete storage response.')
            sequence, status, at, size, reserved = struct.unpack_from('<IIQII', response)
            require(sequence == self.sequence and status == 0 and reserved == 0 and len(response) == 56 + size,
                    'Storage response is uncertain or malformed.')
            require(at == offset, 'Storage response offset differs.')
            return response[56:], response[24:56]
        except BaseException:
            self.failed = True
            raise

    def identify(self):
        data, _ = self.call(1)
        require(data == grant_encode(self.grant), 'Native card/session/range does not match the admitted launch.')


class USBExchange:
    """Use one already-admitted runtime instance under the coordinator's lock.

    The protected launcher owns target/recovery admission and the board lock.
    This adapter accepts its exact node identity; it never discovers, launches,
    resets, retries or reconnects a board. Failure quarantines that instance.
    """
    def __init__(self, access, node, observed_identity, grant):
        import usb.core
        self.access, self.node, self.identity = access, node, observed_identity
        self.grant, self.failed = dict(grant), False
        require(access.identity(node) == observed_identity, 'USB runtime instance changed before connection.')
        access.require_session(node)
        self.device = usb.core.find(bus=int(node.parent.name), address=int(node.name), idVendor=0x1f3a, idProduct=0xefe8)
        require(self.device is not None and self.device.bcdDevice == 0x0102, 'Expected native storage maintenance runtime.')

    def __call__(self, body):
        require(not self.failed, 'A failed USB instance cannot issue another request.')
        sequence, = struct.unpack_from('<I', body, 32)
        packet = struct.pack('<4I', 0x53534f53, sequence, 0x53555044, len(body)) + body
        try:
            require(self.access.identity(self.node) == self.identity, 'USB runtime instance changed.')
            require(self.device.write(0x02, packet, timeout=1500) == len(packet), 'Incomplete storage USB request.')
            response = bytes(self.device.read(0x81, 4096, timeout=1500))
            require(len(response) >= 16 and struct.unpack_from('<4I', response) ==
                    (0x53534f53, sequence, 0xd3555044, len(response) - 16), 'Storage USB response framing differs.')
            return response[16:]
        except BaseException as error:
            self.failed = True
            self.access.quarantine_instance(self.node, str(error), observed_identity=self.identity)
            raise


def one_attempt(operation):
    @functools.wraps(operation)
    def run(client, *args, **kwargs):
        try:
            return operation(client, *args, **kwargs)
        except BaseException:
            client.failed = True
            raise
    return run


@one_attempt
def backup(client, store):
    client.identify()
    fd = store.create('backup.bin')
    sha = hashlib.sha256()
    for offset in range(0, client.grant['blocks'], 2):
        data, expected = client.call(2, offset)
        require(len(data) == min(2, client.grant['blocks'] - offset) * 512 and digest(data) == expected, 'Backup chunk differs from native read.')
        write_all(fd, data)
        sha.update(data)
    whole = sha.digest()
    sealed = store.seal('backup.bin', fd, client.grant['blocks'] * 512, whole)
    manifest = dict(schema='sunstone-storage-backup-1', grant=client.grant, sha256=whole.hex(), bytes=client.grant['blocks'] * 512)
    _, receipt = store.record('backup.json', manifest)
    _, observed = client.call(3, client.grant['blocks'], receipt=receipt)
    require(observed == whole, 'Native and host complete backup hashes differ.')
    if not client.grant['mode']:
        _, observed = client.call(5, client.grant['blocks'])
        require(observed == whole, 'Final backup hash differs.')
    store.record('backup-complete.json', dict(manifest_sha256=receipt.hex(), native_sha256=observed.hex()))
    return sealed, manifest


def prepare(store, original, candidate, launch, qualification, allocation):
    """A trusted bootstrap preparation, with actual protected backup bytes."""
    before = original.json('backup.json')
    original.json('backup-complete.json')
    g = dict(launch)
    require(g['mode'] == 1 and g['nonce'] != before['grant']['nonce'] and card_range(g) == card_range(before['grant']),
            'Write launch must name a fresh session and the backed-up card/range.')
    require(digest(canonical(allocation)).hex() == g['allocation'] and allocation.get('owner') == 'SunstoneOS' and
            allocation.get('card_range') == {k: g[k] for k in ('kind', 'host', 'cid', 'sectors', 'first', 'blocks')},
            'Trusted allocation does not assign this exact card extent to SunstoneOS.')
    # Existing validation checks actual evidence file hashes, not just qualified:true.
    module_path = Path(__file__).with_name('check-a733-smp.py')
    spec = importlib.util.spec_from_file_location('storage_launch_evidence', module_path)
    validator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(validator)
    validator.validate_qualification(qualification, allocation['target']['port'], allocation['target']['sysfs'])
    require(digest(canonical(qualification)).hex() == g['recovery'], 'Recovery artifact differs from immutable grant.')
    store.record('qualification.json', qualification)
    store.record('allocation.json', allocation)
    return _prepare_files(store, original, candidate, g)


def _prepare_files(store, original, candidate, grant):
    """Artifact construction only; callers must separately admit physical launch."""
    before = original.json('backup.json')
    g = dict(grant)
    require(g['mode'] == 1 and card_range(g) == card_range(before['grant']), 'Prepared range differs from backup.')
    prior = original.open('backup.bin', before['bytes'], bytes.fromhex(before['sha256']))
    g['backup'] = hash_fd(prior).hex()
    image, image_hash = snapshot(store, 'candidate.bin', candidate, before['bytes'])
    _, root, chain_hash = make_chain(store, image, before['bytes'])
    g['candidate'] = root.hex()
    # The new job owns a complete independent durable copy of the original backup.
    saved = store.create('backup.bin')
    offset = 0
    while offset < before['bytes']:
        data = os.pread(prior, min(65536, before['bytes'] - offset), offset)
        require(data, 'Backup disappeared during preparation.')
        write_all(saved, data)
        offset += len(data)
    store.seal('backup.bin', saved, before['bytes'], bytes.fromhex(g['backup']))
    raw = grant_encode(g)
    fd = store.create('grant.bin')
    write_all(fd, raw)
    store.seal('grant.bin', fd, len(raw), digest(raw))
    manifest = dict(schema='sunstone-storage-update-1', grant=g, grant_sha256=digest(raw).hex(),
                    bytes=before['bytes'], backup_sha256=g['backup'], candidate_sha256=image_hash.hex(), chain_sha256=chain_hash.hex())
    store.record('prepared.json', manifest)
    return manifest


@one_attempt
def apply(client, store):
    """One connected session. Any uncertainty leaves immutable intents behind."""
    manifest = store.json('prepared.json')
    require(client.grant == manifest['grant'], 'Host client names another prepared session.')
    g = client.grant
    size = manifest['bytes']
    prior = store.open('backup.bin', size, bytes.fromhex(manifest['backup_sha256']))
    image = store.open('candidate.bin', size, bytes.fromhex(manifest['candidate_sha256']))
    chain = store.open('chain.bin', ((size + CHUNK - 1) // CHUNK) * 32, bytes.fromhex(manifest['chain_sha256']))
    client.identify()
    sha = hashlib.sha256()
    for offset in range(0, g['blocks'], 2):
        data, expected = client.call(2, offset)
        require(digest(data) == expected and data == os.pread(prior, len(data), offset * 512) and
                len(data) == min(2, g['blocks'] - offset) * 512, 'Current card differs from protected backup; writes refused.')
        sha.update(data)
    require(sha.hexdigest() == g['backup'], 'Complete current-card backup differs.')
    _, receipt = store.record('admission.json', dict(grant_sha256=manifest['grant_sha256'], backup_sha256=sha.hexdigest(), candidate_sha256=manifest['candidate_sha256']))
    _, observed = client.call(3, g['blocks'], receipt=receipt)
    require(observed == sha.digest(), 'Native backup hash differs.')
    after = hashlib.sha256()
    for index, offset in enumerate(range(0, g['blocks'], 2)):
        data = os.pread(image, min(CHUNK, size - offset * 512), offset * 512)
        following = os.pread(chain, 32, index * 32)
        require(len(data) == min(CHUNK, size - offset * 512) and len(following) == 32, 'Sealed candidate changed.')
        intent = dict(nonce=g['nonce'], sequence=client.sequence + 1, sector=g['first'] + offset, bytes=len(data), sha256=digest(data).hex(), next_commitment=following.hex())
        _, receipt = store.record(f'intent-{index:08d}.json', intent)
        _, readback = client.call(4, offset, data, receipt, following)
        require(readback == digest(data), 'Native readback differs from the candidate.')
        store.record(f'result-{index:08d}.json', dict(intent_sha256=receipt.hex(), readback_sha256=readback.hex()))
        after.update(data)
    _, observed = client.call(5, g['blocks'])
    require(observed == after.digest() and after.hexdigest() == manifest['candidate_sha256'], 'Final readback hash differs.')
    store.record('complete.json', dict(grant_sha256=manifest['grant_sha256'], readback_sha256=observed.hex(), power_loss_durability='unqualified', bootability='unqualified'))


def prepare_restore(store, current_backup, original_backup, launch, qualification, allocation):
    """Explicit restore after a new read-only capture of the uncertain card."""
    original = original_backup.json('backup.json')
    original_backup.json('backup-complete.json')
    require(card_range(original['grant']) == card_range(launch), 'Restore names another card or extent.')
    source = original_backup.open('backup.bin', original['bytes'], bytes.fromhex(original['sha256']))
    result = prepare(store, current_backup, original_backup.path / 'backup.bin', launch, qualification, allocation)
    require(hash_fd(source).hex() == original['sha256'] == result['candidate_sha256'], 'Restoration source changed.')
    store.record('restore.json', dict(original_backup_sha256=original['sha256'],
                 current_backup_sha256=result['backup_sha256'], nonce=launch['nonce']))
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='operation', required=True)
    prep = sub.add_parser('prepare')
    for name in ('backup', 'candidate', 'launch', 'qualification', 'allocation', 'output'):
        prep.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args(argv)
    with ProtectedDirectory(args.backup) as backup_store, ProtectedDirectory(args.output, create=True) as output:
        result = prepare(output, backup_store, args.candidate, json.loads(args.launch.read_text()),
                         json.loads(args.qualification.read_text()), json.loads(args.allocation.read_text()))
    print(json.dumps(result, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
