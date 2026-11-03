#!/usr/bin/env python3
"""Store a Wi-Fi network's credentials where only the board runner reads them.

The user runs this tool in their own terminal. It prompts for the SSID and the
password, derives the WPA2 pairwise master key (PMK) and writes
~/.config/sunstoneos/wifi/NAME.json. Nothing it stores or prints reaches an
agent, the repository, the build cache or the board queue: a submission names
the credential and carries its fingerprint, and the daemon reads the file when
the run starts (docs/wifi-credentials.md).

  wifi-credential.py add NAME          prompt for the SSID and the password and store them
  wifi-credential.py list              print each name and SSID, never a secret
  wifi-credential.py remove NAME       delete the credential file
  wifi-credential.py fingerprint NAME  print the 8-byte fingerprint of the PMK
  wifi-credential.py selftest          check the PMK derivation against the IEEE 802.11i vectors

The runner imports this file for the same derivation, file checks and board
record, so the format has one definition.
"""
import argparse
import base64
import getpass
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys
import uuid

sys.dont_write_bytecode = True

CONFIG_ENVIRONMENT = 'SUNSTONEOS_WIFI_CONFIG_DIR'
NAME = re.compile(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,39}')
SCHEMA = 'sunstoneos-wifi-credential-v1'
# The fingerprint is the first 8 bytes of HMAC-SHA256(key=LABEL, message=PMK).
LABEL = b'sunstoneos wifi credential fingerprint v1'
FINGERPRINT_BYTES = 8
PMK_BYTES = 32
# The record the OS receives (include/sunstone_wifi_credential.h): magic, version, a reserved word, then
# struct ss_wifi_credential, zero padded to RECORD_BYTES.
RECORD_BYTES = 256
RECORD_MAGIC = b'SSWIFIC1'
RECORD_VERSION = 1
FLAG_PMK, FLAG_PASSWORD = 1, 2
# Fixed address in the EL1-only profile span (apps/os/identity.c); the runner uploads the record here.
RECORD_ADDRESS = 0x43004000
# IEEE 802.11i-2004 annex H.4: passphrase, SSID, PSK.
VECTORS = (
    ('password', 'IEEE', 'f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e'),
    ('ThisIsAPassword', 'ThisIsASSID', '0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af'),
    ('a' * 32, 'Z' * 32, 'becb93866bb8c3832cb777c2f559807c8c59afcb6eae734885001300a981cc62'),
)


class CredentialError(ValueError):
    """A refusal whose text never contains a credential value."""


def config_directory():
    override = os.environ.get(CONFIG_ENVIRONMENT)
    return Path(override) if override else Path.home()/'.config/sunstoneos/wifi'


def pmk(password, ssid):
    """WPA2 passphrase to PMK: PBKDF2-HMAC-SHA1, 4096 rounds, the SSID as salt, 32 bytes."""
    return hashlib.pbkdf2_hmac('sha1', password.encode('ascii'), ssid.encode('utf-8'), 4096, PMK_BYTES)


def selftest():
    for password, ssid, expected in VECTORS:
        if pmk(password, ssid).hex() != expected:
            raise CredentialError('PMK derivation differs from the IEEE 802.11i test vector for SSID %r.' % ssid)
    return len(VECTORS)


def fingerprint(key):
    return hmac.new(LABEL, key, hashlib.sha256).digest()[:FINGERPRINT_BYTES].hex()


def check_name(name):
    if not isinstance(name, str) or not NAME.fullmatch(name):
        raise CredentialError('A credential name is 1 to 40 characters of letters, digits, "_", "." and "-", '
                              'starting with a letter or digit.')
    return name


def check_ssid(ssid):
    raw = ssid.encode('utf-8')
    if not 1 <= len(raw) <= 32:
        raise CredentialError('An SSID is 1 to 32 bytes.')
    return ssid


def check_passphrase(password):
    if not (8 <= len(password) <= 63 and all(32 <= ord(c) <= 126 for c in password)):
        raise CredentialError('A WPA2 passphrase is 8 to 63 printable ASCII characters.')
    return password


def require_private(path, mode, kind):
    """Refuse a path that is a link, is not ours, or has any other mode."""
    try:
        status = os.lstat(path)
    except FileNotFoundError:
        raise CredentialError('%s %s does not exist.' % (kind, path)) from None
    wanted = stat.S_IFDIR if kind == 'Directory' else stat.S_IFREG
    if stat.S_IFMT(status.st_mode) != wanted:
        raise CredentialError('%s %s is not a plain %s.' % (kind, path, kind.lower()))
    if status.st_uid != os.getuid():
        raise CredentialError('%s %s is not owned by this user.' % (kind, path))
    if stat.S_IMODE(status.st_mode) != mode:
        raise CredentialError('%s %s has mode %04o; it must be %04o.' % (kind, path, stat.S_IMODE(status.st_mode), mode))


def file_path(name, directory=None):
    return Path(directory or config_directory())/(check_name(name)+'.json')


def load(name, directory=None):
    """The stored entry as a dictionary, after the directory and file modes pass."""
    path = file_path(name, directory)
    require_private(path.parent, 0o700, 'Directory')
    require_private(path, 0o600, 'File')
    try:
        entry = json.loads(path.read_text())
        key = bytes.fromhex(entry['pmk'])
        ssid = check_ssid(entry['ssid'])
        password = entry.get('password')
        assert entry['schema'] == SCHEMA and len(key) == PMK_BYTES
        assert password is None or (isinstance(password, str) and 1 <= len(password.encode()) <= 64)
    except (OSError, ValueError, KeyError, TypeError, AssertionError):
        raise CredentialError('Credential file %s is malformed.' % path) from None
    return {'name': name, 'ssid': ssid, 'pmk': key, 'password': password}


def store(name, ssid, password, store_password, directory=None):
    """Write NAME.json atomically at mode 0600 in a directory at mode 0700."""
    check_ssid(ssid)
    check_passphrase(password)
    selftest()
    directory = Path(directory or config_directory())
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(directory, 0o700)
    require_private(directory, 0o700, 'Directory')
    entry = {'schema': SCHEMA, 'ssid': ssid, 'pmk': pmk(password, ssid).hex()}
    if store_password:
        entry['password'] = password
    path = file_path(name, directory)
    temporary = path.with_name('.'+path.name+'.'+uuid.uuid4().hex+'.tmp')
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        with os.fdopen(descriptor, 'w') as stream:
            json.dump(entry, stream, indent=2, sort_keys=True)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except BaseException:
        # The partial file holds a secret, so it is unlinked here rather than moved anywhere.
        if temporary.exists():
            temporary.unlink()
        raise
    fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)
    return path


def board_record(entry):
    """The 256-byte record the OS reads (struct ss_wifi_credential behind a magic and a version)."""
    ssid = entry['ssid'].encode('utf-8')
    password = (entry.get('password') or '').encode('utf-8')
    flags = FLAG_PMK | (FLAG_PASSWORD if password else 0)
    body = struct.pack('<4I', flags, len(ssid), len(password), 0) + ssid.ljust(32, b'\0') + entry['pmk'] + password.ljust(64, b'\0')
    record = RECORD_MAGIC + struct.pack('<II', RECORD_VERSION, 0) + body
    assert len(record) <= RECORD_BYTES
    return record.ljust(RECORD_BYTES, b'\0')


def secrets_of(entry):
    """Every byte string that must never appear in a queue file, as a scan list (used by checks)."""
    values = [entry['pmk']]
    if entry.get('password'):
        values.append(entry['password'].encode('utf-8'))
    forms = []
    for value in values:
        forms += [value, value.hex().encode(), value.hex().upper().encode(), base64.b64encode(value),
                  base64.urlsafe_b64encode(value)]
    return forms


def confirm(question):
    return input(question+' [y/N] ').strip().lower() in ('y', 'yes')


def add(name, directory=None):
    check_name(name)
    selftest()
    if not (sys.stdin.isatty() and sys.stdout.isatty()):
        raise CredentialError('Run this command in your own terminal: it reads the password from the keyboard '
                              'and refuses a pipe or a script.')
    path = file_path(name, directory)
    if path.exists() and not confirm('%s exists; replace it?' % path.name):
        raise CredentialError('Left %s unchanged.' % path.name)
    ssid = check_ssid(input('SSID: '))
    password = getpass.getpass('Password (not shown): ')
    if getpass.getpass('Password again: ') != password:
        raise CredentialError('The two entries differ.')
    check_passphrase(password)
    keep = confirm('Store the password for WPA3?')
    written = store(name, ssid, password, keep, directory)
    entry = load(name, directory)
    print('Stored %s (SSID %s%s). Fingerprint %s.' % (written.name, ssid, ', password kept for WPA3' if keep else '',
                                                      fingerprint(entry['pmk'])))


def listing(directory=None):
    directory = Path(directory or config_directory())
    rows = []
    if directory.exists():
        require_private(directory, 0o700, 'Directory')
        for path in sorted(directory.glob('*.json')):
            try:
                rows.append((path.stem, load(path.stem, directory)['ssid']))
            except CredentialError as error:
                rows.append((path.stem, '(unreadable: %s)' % error))
    return rows


def remove(name, directory=None):
    path = file_path(name, directory)
    require_private(path.parent, 0o700, 'Directory')
    if not path.exists():
        raise CredentialError('No credential named %s.' % name)
    # The file holds a secret, so it is unlinked rather than moved into any cache or trash directory.
    path.unlink()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--config-dir', type=Path, help='Credential directory (default ~/.config/sunstoneos/wifi).')
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ('add', 'remove', 'fingerprint'):
        commands.add_parser(name).add_argument('name')
    commands.add_parser('list')
    commands.add_parser('selftest')
    args = parser.parse_args(argv)
    try:
        if args.command == 'selftest':
            print('PBKDF2 matches %d IEEE 802.11i test vectors' % selftest())
        elif args.command == 'add':
            add(args.name, args.config_dir)
        elif args.command == 'list':
            for name, ssid in listing(args.config_dir):
                print('%-20s %s' % (name, ssid))
        elif args.command == 'remove':
            remove(args.name, args.config_dir)
            print('Removed %s.' % args.name)
        else:
            print(fingerprint(load(args.name, args.config_dir)['pmk']))
    except CredentialError as error:
        print(error, file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
