#!/usr/bin/env python3
"""Read the 32-bit words of one Read bundle through the boot ROM's FEL interface.

Usage (under the coordinator's bundle-run.py, which sets SUNSTONE_USB_PORT):
  board-read.py WORDS.json EVIDENCE_DIR

tools/prepare-board-bundles.py ships this file unchanged as board-read.py in
every Read bundle (docs/designs/board-bundles-next.md). WORDS.json is data
only: each word has either a block with no software gate (CCU, R_CCU, SID or
the CPU PLL block) or a condition word listed earlier, with a mask and a
value. The reader:

- saves the 128 FEL scratch bytes at 0x71000 with read; each readl command
  writes a 48-byte A32 stub and the word's slot, the same first 52 bytes, and
  runs the stub in the ROM's context, and the first FEL write of a session
  also clears SCTLR.I there (aw_disable_icache);
- reads each condition word in its own readl command, compares it under its
  mask, and skips every dependent of a condition that does not match;
- reads the other words of a gate together, one readl command per word in one
  sunxi-fel process;
- restores the scratch bytes with write, reads them back with read in the same
  process, and compares them.

It issues no other FEL command, writes no device register and never retries.
A stalled or aborted read leaves the ROM unable to answer, and the board then
needs a power removal; the reader keeps the port lock and quarantines the
instance after any failure.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

SCRATCH = 0x71000
SCRATCH_BYTES = 128
SCHEMA = 'board-read-words-v1'
# Blocks without a software clock gate or reset, with the pinned sources that
# show it (docs/designs/board-bundles-next.md, "Gates").
UNGATED = {
    'ccu': (0x02002000, 0x2000),       # sun60iw2p1.dtsi lines 917-919
    'r_ccu': (0x07010000, 0x340),      # sun60iw2p1.dtsi lines 938-940
    'sid': (0x03006000, 0x1000),       # sun60iw2p1.dtsi lines 1406-1408
    'cpu_pll': (0x08870000, 0x3034),   # sun60iw2p1.dtsi lines 908-915
}
WORD_KEYS = {'address', 'block', 'condition', 'note'}
CONDITION_KEYS = {'address', 'mask', 'value'}
FILE_KEYS = {'schema', 'purpose', 'words'}


class Refused(ValueError):
    pass


def number(text, what):
    if not (isinstance(text, str) and text.startswith('0x')):
        raise Refused(f'{what} must be a hexadecimal string: {text!r}')
    value = int(text, 16)
    if not 0 <= value < 1 << 32:
        raise Refused(f'{what} lies outside 32 bits: {text}')
    return value


def plan(value):
    """The validated word list: (address, block, condition or None, note) in read order.

    Refuses any key outside the schema, an unaligned or repeated address, an
    ungated word outside its named block, and a condition that is not an
    earlier word of the list.
    """
    if not isinstance(value, dict) or set(value) != FILE_KEYS or value['schema'] != SCHEMA:
        raise Refused(f'WORDS.json must hold exactly {sorted(FILE_KEYS)} with schema {SCHEMA}.')
    if not isinstance(value['purpose'], str) or not value['purpose']:
        raise Refused('WORDS.json names no purpose.')
    words = value['words']
    if not isinstance(words, list) or not words:
        raise Refused('WORDS.json lists no word.')
    seen, result = set(), []
    for item in words:
        if not isinstance(item, dict) or 'address' not in item or not set(item) <= WORD_KEYS:
            raise Refused(f'A word holds keys outside {sorted(WORD_KEYS)}: {item!r}')
        address = number(item['address'], 'address')
        if address & 3:
            raise Refused(f'Word {item["address"]} is not 32-bit aligned.')
        if address in seen:
            raise Refused(f'Word {item["address"]} repeats.')
        note = item.get('note', '')
        if not isinstance(note, str):
            raise Refused(f'Word {item["address"]} has a note that is not text.')
        if ('block' in item) == ('condition' in item):
            raise Refused(f'Word {item["address"]} needs exactly one of an ungated block or a condition.')
        condition = None
        if 'block' in item:
            block = UNGATED.get(item['block'])
            if block is None or not block[0] <= address < block[0] + block[1]:
                raise Refused(f'Word {item["address"]} is not inside the ungated block {item["block"]!r}.')
        else:
            gate = item['condition']
            if not isinstance(gate, dict) or set(gate) != CONDITION_KEYS:
                raise Refused(f'Word {item["address"]} has a condition other than {sorted(CONDITION_KEYS)}.')
            condition = tuple(number(gate[k], k) for k in ('address', 'mask', 'value'))
            if condition[0] not in seen:
                raise Refused(f'Word {item["address"]} depends on {gate["address"]}, which is not an earlier word.')
            if condition[1] == 0 or condition[2] & ~condition[1]:
                raise Refused(f'Word {item["address"]} has an empty mask or a value outside it.')
        seen.add(address)
        result.append((address, item.get('block'), condition, note))
    return result


def capture(words, tx, readl, record, save):
    """Read every admitted word in order; tx and readl perform the FEL commands.

    readl(addresses) returns one value per address from one sunxi-fel process.
    """
    gates = {c[0] for _, _, c, _ in words if c}
    values, pending, pending_gate = {}, [], None

    def flush():
        nonlocal pending
        if pending:
            for address, value in zip(pending, readl(pending)):
                values[address] = value
            pending = []

    for address, _, condition, _ in words:
        if condition is not None:
            source, mask, expected = condition
            if source not in values:
                record['skipped'].append({'address': hex(address), 'reason': f'{hex(source)} was skipped'})
                save()
                continue
            if values[source] & mask != expected:
                record['skipped'].append({'address': hex(address), 'reason': f'{hex(source)} & {hex(mask)} is '
                                          f'{hex(values[source] & mask)}, not {hex(expected)}'})
                save()
                continue
        if address in gates:
            # A condition word is read in its own command and process.
            flush()
            values[address] = readl([address])[0]
            continue
        if condition != pending_gate:
            flush()
            pending_gate = condition
        pending.append(address)
    flush()
    record['words'] = {hex(a): values[a] for a, _, _, _ in words if a in values}
    save()
    return values


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    words_path, evidence = Path(sys.argv[1]), Path(sys.argv[2])
    raw = words_path.read_bytes()
    words = plan(json.loads(raw))
    here = Path(__file__).resolve().parent
    tools = here / 'host/tools' if (here / 'host/tools').is_dir() else here
    b = module('board_read_base', tools / 'check-a733-smp.py')
    a = module('board_read_access', tools / 'usb-access.py')
    fel = Path.home() / '.cache/sunstoneos/build/sunxi-a733/sunxi-fel'
    b.require(b.digest(fel) == b.FEL_HASH, 'FEL changed')
    evidence.mkdir(parents=True, exist_ok=False)
    r = {'schema': 'board-read-session-v1', 'complete': False, 'launch_attempted': False, 'physical_access': True,
         'reader_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
         'words_sha256': hashlib.sha256(raw).hexdigest(), 'transactions': [], 'words': {}, 'skipped': [],
         'scratch_restored': False}

    def save():
        b.save_json(evidence / 'session.json', r)

    with a.board_lock() as node:
        a.require_session(node)
        a.require_rom(node)
        identity = a.identity(node)
        r.update(identity=identity, node=str(node), port=a.target(node).name)
        save()
        device = f'{int(node.parent.name)}:{int(node.name)}'

        def tx(*args, pass_fds=()):
            a.require_rom(node)
            b.require(a.identity(node) == identity, 'Instance changed')
            command = [str(fel), '-d', device, *map(str, args)]
            entry = {'command': command}
            r['transactions'].append(entry)
            save()
            x = subprocess.run(command, capture_output=True, text=True, timeout=30, pass_fds=pass_fds)
            entry.update(exit_code=x.returncode, stdout=x.stdout, stderr=x.stderr)
            save()
            b.require(x.returncode == 0, 'FEL transaction failed; no retry')
            return x.stdout

        def readl(addresses):
            args = []
            for address in addresses:
                args += ['readl', hex(address)]
            values = [int(v, 16) for v in tx(*args).split()]
            b.require(len(values) == len(addresses), 'Unexpected result length')
            return values

        try:
            backup = evidence / 'scratch-before.bin'
            tx('read', hex(SCRATCH), str(SCRATCH_BYTES), backup)
            before = backup.read_bytes()
            b.require(len(before) == SCRATCH_BYTES, 'Short scratch backup')
            b.sync_file(backup)
            capture(words, tx, readl, r, save)
            with b.sealed_source(before) as (path, fd):
                tx('write', hex(SCRATCH), path, 'read', hex(SCRATCH), str(SCRATCH_BYTES),
                   evidence / 'scratch-restored.bin', pass_fds=(fd,))
            b.require((evidence / 'scratch-restored.bin').read_bytes() == before, 'Scratch restoration failed')
            r['scratch_restored'] = True
            r['complete'] = True
            save()
        except BaseException as error:
            try:
                b.stop_after_failure(r, save, lambda: a.quarantine_instance(
                    node, 'Board read stopped; no retry; coordinator recovery required.', identity), error)
            finally:
                b.hold_ownership()
            raise
    print(json.dumps({'port': r['port'], 'complete': True, 'words': len(r['words']), 'skipped': len(r['skipped'])}))


if __name__ == '__main__':
    main()
