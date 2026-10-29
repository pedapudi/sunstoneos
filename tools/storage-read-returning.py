#!/usr/bin/env python3
"""Host side of the returning SD0 read image (apps/storage-read-returning).

  storage-read-returning.py sign --approval APPROVAL --ddr DDR_DIR --survey SURVEY_DIR --signer NAME
  storage-read-returning.py input --approval APPROVAL --request REQUEST --ddr DDR_DIR --survey SURVEY_DIR
                                  --fabric-read READ_DIR --config CONFIG --profile 2|3
                                  --plan measurement|first-read|chain-read --output INPUT.bin
  storage-read-returning.py decode --run RUN_DIR --input INPUT.bin --output RESULT.json

sign records, in the coordinator's approval of one lifetime, the SHA-256 of
that lifetime's original cold DDR start and current survey and the signing time. It
refuses a DDR start or survey that did not pass, a survey on another port than
3-3 or on another node or ROM identity than the DDR start, and an approval that
is already signed, so no approval carries into the next lifetime. Repeat
--warm-return SESSION_JSON on sign and input to bind an ordered restored
no-provider entry-checkpoint chain from that DDR instance to the current survey.

input writes the 2192-byte input that the image reads at 0x43000000
(apps/storage-read-returning/sdrt.h): SD profile 2 or 3, fresh owner, handoff
and topology identifiers, the fabric words the image will find, the PMIC and PF pin
configurations of CONFIG, and, with --plan measurement (the default), a 240-read DMA plan: sequential and random
4 KiB, 32 KiB and 128 KiB requests, plus single sectors. The first 64 reads
repeat the accepted PIO run; the largest request is 256 blocks. It refuses unless the approval is signed for this DDR
start and survey, newer than the DDR record, names the profile's bundle and
sets every field that approval-request.json lists for it to true.
--plan first-read requires --profile 2 and reads only sector 0, eight blocks
(4096 bytes). It uses the same lifetime admission and native input format.
--plan chain-read requires --profile 2 and reads 128 sectors from sector zero
(65536 bytes), using sixteen descriptors and sixteen per-4096-byte CRC records.

The fabric words come from this lifetime's own read of CCU 0x500, 0x580 and
0x588 (READ_DIR, the board-read-session-v1 record that
tools/board-bundles-next-read.py writes, on the admitted current node and ROM
identity), masked and converted to rates as the SD0 provider does. Every
image's EL3 monitor restores MBUS before EL1 starts
(include/a733_el3_mbus.h): when the MBUS word under A733_MBUS_FIELDS equals
the ROM's A733_MBUS_ROM (400 MHz), it sets those fields to boot0's
A733_MBUS_BOOT0 (600 MHz). The ROM sets 400 MHz again on every return, so a
read before the launch sees it while the image sees the restored word; the
tool applies the same rule with the constants read from that header. The
monitor also requires MSI_LITE0 released and the GPU, NPU, DMA0 and CE idle,
which a FEL read before the launch does not see. When it skips the restore
for any of these, the SD0 provider refuses the predicted MBUS word and the
image stops (SD_REASON 3); the monitor writes its MBUS line only for an
active bus master (codes 8 to 10) or a failure, not for the other skips.

decode reads the runner's kernel.log and result record, checks every read line
against the input's plan, and writes the card registers, the selected mode and
per-class throughput, observed command/data phases and sequential fixed-cost
and per-block fits with and without cache costs. Saturated timings are excluded.
Version-1 PIO captures remain decodable; --reference checks the repeated CRCs
against a decoded accepted PIO run on the same card. Successful first-read
decoding requires --reference and compares its first 4096-byte read;
measurement plans still require 64 matching reads. --require-restored-return
also requires the input producer's explicit matching restored-return policy
provenance and exact normal-return receipts before emitting successor eligibility.
The coordinator binds the frozen image, signed approval and returned ROM record.
No board, USB or queue access.
"""
import argparse
import hashlib
import importlib.util
import json
from datetime import datetime, timezone
from pathlib import Path
import re
import secrets
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
MAGIC = 0x314e525445524453  # "SDRETRN1", apps/storage-read-returning/sdrt.h
VERSION = 2
INPUT_BYTES = 2192
PLAN = 256
PORT = '3-3'
COMPLETE = 'COMPLETE SD0 returning read'
INCOMPLETE = 'INCOMPLETE SD0 returning read: '
BUNDLES = {2: 'A', 3: 'B'}
FIRST_READ_PLAN = [(0, 8)]
CHAIN_READ_PLAN = [(0, 128)]
POWER_REMOVAL = 'power-removal'
RESTORED_RETURN = 'restored-3v3-first-read'
CHAIN_RETURN = 'restored-3v3-chain-read'
RETURN_POLICIES = (POWER_REMOVAL, RESTORED_RETURN, CHAIN_RETURN)
LEGACY_RECOVERY = {'rom_return_state_accepted', 'no_launch_or_reset_before_power_removal',
                   'whole_board_power_removal_after_lifetime'}
RESTORED_RECOVERY = ('rom_return_state_accepted', 'reviewed_3v3_normal_return',
                     'successor_only_after_exact_image_and_reference_validation',
                     'retain_no_launch_or_reset_until_power_removal_on_failure_or_uncertainty')
# The SD0 provider's fabric words, masks and parent rates in MHz
# (drivers/storage/a733_mmc_platform.c: fabric_off, fabric_mask, parent_mhz).
# The image only reads these words; no unit of it writes CCU 0x500, 0x580 or 0x588.
FABRIC = (0x02002500, 0x02002580, 0x02002588)
READ_SCHEMA = 'board-read-session-v1'  # tools/board-bundles-next-read.py, main()


def header_constants(path=ROOT / 'include/a733_el3_mbus.h'):
    """The #define NAME UINT32_C(0x...) constants of the monitor's MBUS header."""
    found = re.findall(r'^#define (A733_\w+) UINT32_C\((0x[0-9a-fA-F]+)\)$', Path(path).read_text(), re.M)
    return {name: int(value, 16) for name, value in found}


_MBUS = header_constants()
MBUS_FIELDS, MBUS_ROM, MBUS_RESTORED = _MBUS['A733_MBUS_FIELDS'], _MBUS['A733_MBUS_ROM'], _MBUS['A733_MBUS_BOOT0']
FABRIC_MASK = (0x0300001f, 0x8700001f, 0x8700001f)
PARENT_MHZ = ((24, 0, 0, 600, 0, 0, 0, 0), (24, 0, 800, 600, 480, 0, 0, 0), (24, 600, 0, 480, 400, 0, 0, 0))


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


def require(ok, message):
    if not ok: raise ValueError(message)


def digest(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def throughput():
    """The supervised checker's read plan and summary, unchanged."""
    return module('storage_read_returning_checker', ROOT / 'tools/check-a733-storage-read-supervised.py')


# The first 64 requests repeat the accepted programmed-I/O run.
DMA_CLASSES = ((64, 8, 'sequential-4k'), (64, 8, 'random-4k'), (32, 64, 'sequential-32k'),
               (32, 64, 'random-32k'), (8, 256, 'sequential-128k'), (8, 256, 'random-128k'), (32, 1, 'single'))


def dma_plan():
    plan = []; state = 4194304
    for count, blocks, name in DMA_CLASSES:
        for i in range(count):
            if name.startswith('random'):
                state = (state * 1103515245 + 12345) & 0x7fffffff
                first = blocks * (state % (4194304 // blocks))
            else:
                first = i * blocks
            plan.append((first, blocks))
    return plan


def dma_summary(reads, hz):
    """Observed costs; cache-subtracted values are estimates, not bus measurements."""
    require([(r['first'], r['blocks']) for r in reads] == dma_plan(), 'DMA throughput plan mismatch')
    out = {}; index = 0; sequential = []
    for count, blocks, name in DMA_CLASSES:
        part = reads[index:index + count]; index += count
        observed=[r for r in part if not r['timing_saturated']]
        elapsed=sum(r['elapsed_us'] for r in observed)
        require(hz>0, 'zero card clock rate')
        out[name]=dict(requests=count, measured_requests=len(observed), saturated_requests=count-len(observed),
                       bytes=512*blocks*len(observed), elapsed_us=elapsed,
                       mb_per_s=round(512*blocks*len(observed)/elapsed,3) if elapsed else None,
                       mean_command_to_first_us=(sum(r['first_byte_us']-r['command_us'] for r in observed)/len(observed)
                                                 if observed else None),
                       mean_data_us=(sum(r['done_us']-r['first_byte_us'] for r in observed)/len(observed) if observed else None),
                       bus_us_per_block=1042*1000000/hz)
        if name.startswith('sequential') or name == 'single': sequential += part
    def fit(cache):
        points = [(r['blocks'], r['elapsed_us'] - (r['clean_us'] + r['invalidate_us'] if cache else 0))
                  for r in sequential if not r['timing_saturated']]
        if len(points)<=1: return None
        mx = sum(x for x, y in points) / len(points); my = sum(y for x, y in points) / len(points)
        variance = sum((x - mx)**2 for x, y in points)
        if variance<=0: return None
        slope = sum((x - mx) * (y - my) for x, y in points) / variance
        return dict(per_request_us=my - slope * mx, per_block_us=slope, requests=len(points))
    out['fit_with_cache'] = fit(False); out['fit_without_cache'] = fit(True)
    return out


def reference_card(value, reference):
    """Require complete results and the same identified card."""
    require(isinstance(value, dict) and value.get('complete') is True and
            isinstance(reference, dict) and reference.get('complete') is True,
            'result and reference must be complete')
    card, prior_card = value.get('card'), reference.get('card')
    require(isinstance(card, dict) and isinstance(prior_card, dict) and
            isinstance(card.get('cid'), str) and re.fullmatch(r'[0-9a-fA-F]{32}', card['cid']) and
            prior_card.get('cid') == card['cid'],
            'reference must be complete and name the same card')
    return card, prior_card


def compare_reference(value, reference, first_read=False):
    """Match repeated sectors and card identity against a decoded accepted PIO run."""
    card, prior_card = reference_card(value, reference)
    count = 1 if first_read else 64
    mine, prior = value.get('reads'), reference.get('reads')
    require(isinstance(mine, list) and isinstance(prior, list) and
            len(mine) >= count and len(prior) >= count, 'reference read coverage')
    if first_read:
        require(len(mine) == 1 and value.get('profile') == 2 and card.get('profile') == 2 and
                reference.get('status') == 0 and prior_card.get('profile') == 2,
                'first-read requires a successful profile-2 reference')
    for a, b in zip(mine[:count], prior[:count]):
        require(all(isinstance(r, dict) and all(type(r.get(k)) is int for k in ('first', 'blocks', 'bytes', 'crc32'))
                    and 0 <= r['first'] < 2**32 and r['blocks'] == 8 and r['bytes'] == 4096 and
                    0 <= r['crc32'] < 2**32 for r in (a, b)), 'malformed reference read')
        require(not first_read or a['first'] == b['first'] == 0, 'first-read reference sector')
        require((a['first'], a['crc32']) == (b['first'], b['crc32']), 'reference CRC mismatch')
    return {'matched_reads': count, 'matched_bytes': count * 4096}


def compare_chain_reference(value, reference):
    """Compare every 4 KiB of one 64 KiB DMA read with accepted PIO data."""
    card, prior_card = reference_card(value, reference)
    require(value.get('status') == 0 and value.get('profile') == card.get('profile') == 2 and
            reference.get('status') == 0 and prior_card.get('profile') == 2,
            'chain-read requires a successful profile-2 reference')
    reads, prior, chunks = value.get('reads'), reference.get('reads'), value.get('chunk_crc32')
    require(isinstance(reads, list) and len(reads) == 1 and
            (reads[0].get('first'), reads[0].get('blocks'), reads[0].get('bytes')) == (0, 128, 65536),
            'chain-read extent')
    require(isinstance(chunks, list) and len(chunks) == 16 and
            isinstance(prior, list) and len(prior) >= 16, 'chain-read reference coverage')
    for index, (crc, row) in enumerate(zip(chunks, prior[:16])):
        require(type(crc) is int and 0 <= crc < 2**32 and isinstance(row, dict) and
                all(type(row.get(k)) is int for k in ('first', 'blocks', 'bytes', 'crc32')) and
                (row['first'], row['blocks'], row['bytes']) == (index * 8, 8, 4096) and
                0 <= row['crc32'] < 2**32, 'malformed chain-read reference')
        require(crc == row['crc32'], 'chain-read reference CRC mismatch')
    return {'matched_reads': 16, 'matched_bytes': 65536, 'dma_reads': 1}


def lifetime(ddr, survey, warm_returns=()):
    """Effective (identity, node) from original DDR, ordered restored checkpoints and current survey."""
    ddr_path, survey_path = Path(ddr) / 'session.json', Path(survey) / 'session.json'
    d = json.loads(ddr_path.read_text()); s = json.loads(survey_path.read_text())
    result = d.get('result') or {}
    require(d.get('complete') is True and result.get('stage') == 5 and result.get('board_result') == 0 and
            isinstance(result.get('dram_mib'), int) and result['dram_mib'] >= 64, 'DDR start did not pass')
    identity, node = d.get('node_identity'), d.get('device')
    require(isinstance(identity, list) and len(identity) == 5 and isinstance(node, str) and
            node.startswith('/dev/bus/usb/'), 'DDR start names no USB instance')
    gates = (s.get('result') or {}).get('launcher_gates') or {}
    require(s.get('complete') is True and s.get('sram_restored') is True and s.get('rom_usable') is True and
            gates and all(v is True for v in gates.values()), 'survey did not pass')
    require(s.get('port') == PORT, 'survey ran on another port than 3-3')
    seen = s.get('identity'); seen = json.loads(seen) if isinstance(seen, str) else seen
    if warm_returns:
        raw = []
        for path in warm_returns:
            data = Path(path).read_bytes(); link = json.loads(data)
            npu = link.get('npu_result') or {}; mbus = npu.get('mbus') or {}
            require(link.get('execution_profile') == 'npu-entry-mbus-checkpoint-v1' and
                    link.get('dirty') is False and link.get('dram_restored') is True and
                    link.get('rom_usable') is True and link.get('ownership_release_permitted') is True and
                    npu.get('checkpoint') == 'ENTRY_MEMORY_READY' and npu.get('normal_return') is True and
                    type(npu.get('jobs')) is int and npu['jobs'] == 0 and npu.get('status') == 0 and
                    type(mbus.get('restore_result')) is int and mbus['restore_result'] == 0,
                    'warm return is not a quiescent restored entry checkpoint')
            raw.append(data)
        runner = module('sd_return_continuity', ROOT / 'tools/check-a733-kernel.py')
        base = module('sd_return_continuity_base', ROOT / 'tools/check-a733-smp.py')
        try:
            runner.validate_platform_admission((ddr_path.read_bytes(), survey_path.read_bytes()),
                Path(s['node']), seen, PORT, d.get('tool_sha256'), base.FEL_HASH, raw)
        except RuntimeError as error:
            raise ValueError(str(error)) from error
        identity, node = seen, s['node']
    else:
        require(s.get('node') == node and seen == identity, 'survey ran on another node or ROM identity than the DDR start')
    require(survey_path.stat().st_mtime > max([ddr_path.stat().st_mtime] +
            [Path(p).stat().st_mtime for p in warm_returns]), 'survey is older than the DDR start or warm return')
    return identity, node


def warm_binding(warm_returns, survey):
    if not warm_returns: return {}
    current = json.loads((Path(survey) / 'session.json').read_text())
    identity = current.get('identity')
    if isinstance(identity, str): identity = json.loads(identity)
    return dict(warm_return_sha256=[digest(p) for p in warm_returns],
                effective_node=current.get('node'), effective_identity=identity)


def sign(approval, ddr, survey, signer, return_policy=POWER_REMOVAL, request=None, warm_returns=()):
    lifetime(ddr, survey, warm_returns)
    value = json.loads(Path(approval).read_text())
    require(signer and value.get('reviewer') and value.get('reviewed_at') and value.get('bundle') in BUNDLES.values(),
            'approval needs its bundle, reviewer, reviewed_at and a signer')
    require(not value.get('signature'), 'approval is already signed for a lifetime')
    value['signature'] = {'signer': signer, 'signed_at': datetime.now(timezone.utc).isoformat(),
                          'ddr_session_sha256': digest(Path(ddr) / 'session.json'),
                          'survey_session_sha256': digest(Path(survey) / 'session.json')}
    value['signature'].update(warm_binding(warm_returns, survey))
    if return_policy != POWER_REMOVAL:
        require(return_policy in (RESTORED_RETURN, CHAIN_RETURN) and value['bundle'] == 'A' and request is not None,
                'restored return requires bundle A and --request')
        recovery_fields(request, 'A', return_policy)
        value['signature'].update(return_policy=return_policy, request_sha256=digest(request))
    Path(approval).write_text(json.dumps(value, indent=1) + '\n')
    return value['signature']


def recovery_fields(request, bundle, return_policy):
    """Replace only the known legacy recovery obligations, explicitly and once."""
    fields = json.loads(Path(request).read_text())['fields'][bundle]
    if return_policy in (RESTORED_RETURN, CHAIN_RETURN):
        require(bundle == 'A' and set(fields.get('recovery', ())) == LEGACY_RECOVERY,
                'restored return requires the exact reviewed bundle-A recovery request')
        fields['recovery'] = RESTORED_RECOVERY
    else:
        require(return_policy == POWER_REMOVAL, 'unsupported return policy')
    return fields


def approved(approval, request, ddr, survey, bundle, return_policy=POWER_REMOVAL, warm_returns=()):
    """The approval's signature binds this lifetime and every requested field is true."""
    value = json.loads(Path(approval).read_text())
    fields = recovery_fields(request, bundle, return_policy)
    require(value.get('bundle') == bundle and value.get('reviewer') and value.get('reviewed_at'),
            'approval names another bundle or no reviewer')
    signature = value.get('signature') or {}
    binding = warm_binding(warm_returns, survey)
    require(signature.get('warm_return_sha256', []) == binding.get('warm_return_sha256', []) and
            all(signature.get(k) == v for k, v in binding.items()), 'approval signed for another warm-return chain')
    require(signature.get('return_policy', POWER_REMOVAL) == return_policy,
            'approval signed for another return policy')
    if return_policy in (RESTORED_RETURN, CHAIN_RETURN):
        require(signature.get('request_sha256') == digest(request), 'approval signed for another request')
    ddr_path = Path(ddr) / 'session.json'
    require(signature.get('ddr_session_sha256') == digest(ddr_path) and
            signature.get('survey_session_sha256') == digest(Path(survey) / 'session.json'),
            'approval is not signed for this lifetime\'s DDR start and survey')
    started = ddr_path.stat().st_mtime
    require(datetime.fromisoformat(signature['signed_at']).timestamp() > started and
            Path(approval).stat().st_mtime > started, 'approval is older than this lifetime\'s DDR start')
    for record, names in fields.items():
        for name in names:
            require(value.get('fields', {}).get(record, {}).get(name) is True, f'{record}.{name} not approved')
    return value


def fabric(words):
    """(config words, rates in Hz) from the three register words, as the provider checks them:
    each word under its mask, with bit 31 set in the second and third and a known parent."""
    config, rates = [], []
    for i, word in enumerate(words):
        require(type(word) is int and 0 <= word < 2**32, 'invalid fabric word')
        value = word & FABRIC_MASK[i]
        mhz, div = PARENT_MHZ[i][(value >> 24) & 7], (value & 31) + 1
        require(mhz and (mhz * 1000000) % div == 0 and (i == 0 or value >> 31),
                f'fabric word {i} 0x{word:08x} has no admitted parent or gate')
        config.append(value); rates.append(mhz * 1000000 // div)
    return config, rates


def restored(words):
    """The words the image finds after the monitor's MBUS restore (a733_el3_mbus_restore): the MBUS
    word's fields become boot0's value when they hold the ROM's; every other word stays."""
    return [(w & ~MBUS_FIELDS) | MBUS_RESTORED if i == 2 and w & MBUS_FIELDS == MBUS_ROM else w
            for i, w in enumerate(words)]


def fabric_read(read, ddr, warm_returns=()):
    """(the image's fabric configuration, the read words) of this lifetime's own read of the three words."""
    path, ddr_path = Path(read) / 'session.json', Path(ddr) / 'session.json'
    record, d = json.loads(path.read_text()), json.loads(ddr_path.read_text())
    seen = record.get('identity'); seen = json.loads(seen) if isinstance(seen, str) else seen
    require(record.get('schema') == READ_SCHEMA and record.get('complete') is True and
            record.get('scratch_restored') is True and record.get('port') == PORT, 'fabric read did not pass on 3-3')
    current = json.loads(Path(warm_returns[-1]).read_text()) if warm_returns else {}
    require(record.get('node') == current.get('returned_node', d.get('device')) and
            seen == current.get('returned_identity', d.get('node_identity')),
            'fabric read ran on another node or ROM identity than the DDR start')
    require(path.stat().st_mtime > max([ddr_path.stat().st_mtime] +
            [Path(p).stat().st_mtime for p in warm_returns]), 'fabric read is older than the DDR start or warm return')
    words = {int(k, 16): v for k, v in (record.get('words') or {}).items()}
    require(all(a in words for a in FABRIC), 'fabric read lacks a fabric word')
    read_words = [words[a] for a in FABRIC]
    return fabric(restored(read_words)), read_words


def pins(values, count):
    require(len(values) == count and all(len(v) == 3 and all(type(x) is int and 0 <= x <= 15 for x in v)
                                         for v in values), 'invalid pin configuration')
    return bytes(x for v in values for x in v)


def input_bytes(profile, generation, config, plan, ids=None):
    """The image's input (sdrt.h): header, configuration and plan."""
    require(profile in BUNDLES and type(generation) is int and 0 < generation < 2**64, 'invalid profile or generation')
    require(0 < len(plan) <= PLAN and all(0 <= f < 2**32 and 1 <= b <= 256 and f+b <= 2**32 for f, b in plan), 'invalid plan')
    owner, handoff, topology = ids or [secrets.randbits(63) | 1 for _ in range(3)]
    for name, count in (('fabric', 3), ('fabric_hz', 3)):
        require(len(config[name]) == count and all(type(v) is int and 0 <= v < 2**32 for v in config[name]),
                'invalid ' + name)
    raw = struct.pack('<4Q3Q4I3I3I', MAGIC, VERSION, INPUT_BYTES, generation, owner, handoff, topology,
                      profile, 0x00300000, len(plan), 0, *config['fabric'], *config['fabric_hz'])
    raw += pins(config['pmic_pins'], 2) + pins(config['expected_pins'], 7) + pins(config['desired_pins'], 7)
    require(len(raw) == 144, 'input header size')
    raw += b''.join(struct.pack('<2I', f, b) for f, b in plan) + bytes(8 * (PLAN - len(plan)))
    require(len(raw) == INPUT_BYTES, 'input size')
    return raw


def parse_input(raw):
    require(len(raw) == INPUT_BYTES, 'input size')
    magic, version, size, generation, owner, handoff, topology, profile, mask, reads, _ = struct.unpack_from('<4Q3Q4I', raw)
    require(magic == MAGIC and version in (1, VERSION) and size == INPUT_BYTES and 0 < reads <= PLAN, 'not an SD0 returning input')
    require(generation and owner and owner != 2 and handoff and topology, 'invalid input identities')
    plan = [struct.unpack_from('<2I', raw, 144 + 8 * i) for i in range(reads)]
    require(profile in BUNDLES and all(0 <= f < 2**32 and 1 <= b <= (8 if version == 1 else 256) and f+b <= 2**32 for f,b in plan), 'invalid plan')
    return {'version': version, 'generation': generation, 'profile': profile, 'voltage_mask': mask, 'plan': plan}


# SD_REASON values (apps/storage-read-returning/README.md); SD_STAGE values
# (sdrt.h, enum sdrt_stage); SD_CORE_PROTOCOL values (drivers/storage/sunxi_mmc.c).
REASONS = {1: 'end deadline or provider failure', 2: 'provider construction', 3: 'identification or mode selection',
           4: 'card registers', 5: 'read submission', 6: 'read in progress', 7: 'read result',
           9: 'PMIC not parked or request outstanding', 10: 'card clock stop not confirmed'}
STAGES = ('construct', 'identify', 'card', 'read', 'poll', 'take')
PROTOCOLS = ('IDLE', 'SD_IF', 'SD_APP', 'SD_OCR', 'MMC_QUERY', 'MMC_OCR', 'CID', 'RCA', 'CSD', 'SELECT', 'STATUS',
             'BLOCKLEN', 'EXT', 'EXT_STATUS', 'READ', 'READ_STATUS', 'WRITE_PRE', 'WRITE', 'WRITE_STATUS',
             'SD_SCR_APP', 'SD_SCR', 'SD_WIDTH_APP', 'SD_WIDTH', 'SD_SWITCH_CHECK', 'SD_SWITCH_SET', 'SD_CLOCK',
             'SD_MODE_STATUS', 'SD_VOLTAGE', 'SD_SIGNAL', 'SD_UHS_CHECK', 'SD_UHS_SET', 'SD_TUNE', 'SD_TUNE_RECOVER')
LINE = re.compile(r'R ([0-9a-f]{3}) ([0-9a-f]{8}) ([0-9a-f]{2}) ([0-9a-f]{8}) ([0-9a-f]{8})')
DMA_LINE = re.compile(r'R ([0-9a-f]{3}) ([0-9a-f]{5}) ([0-9a-f]{4}) ([0-9a-f]{5}) ([0-9a-f]{5}) ([0-9a-f]{3}) ([0-9a-f]{3}) ([0-9a-f]{4}) ([0-9a-f]{8})')
CARD = re.compile(r'CARD ([0-9a-f]{32}) ([0-9a-f]{32}) ([0-9a-f]{8}) ([0-9a-f]{4}) ([0-9a-f]{16}) ([0-9a-f]{16}) '
                  r'([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{16}) ([0-9a-f]{8})')


def decode_log(log, raw_input, status=None):
    """The reads, card and mode of one log, checked against the input that produced it."""
    given = parse_input(raw_input); plan = given['plan']
    lines = log.splitlines()
    value = {'status': status, 'profile': given['profile'], 'generation': given['generation'], 'complete': False, 'mbus': None,
             'outcome': None, 'reads': [], 'card': None, 'first_sector_hex': None}
    chunks = []
    words = {}
    for line in lines:
        if line.startswith(('SD_', 'SD0 returning start')) and '=' in line:
            key, number = line.split('=', 1)
            require(key not in words,'duplicate result word: '+key)
            words[key] = int(number, 16)
        elif line.startswith('R '):
            match = (LINE if given['version']==1 else DMA_LINE).fullmatch(line)
            require(match is not None, 'malformed read line: ' + line)
            fields = [int(x,16) for x in match.groups()];index=fields[0]
            require(index == len(value['reads']) and index < len(plan), f'read line {index} differs from the input plan')
            first,blocks=plan[index]
            if given['version']==1:
                _,f,b,elapsed,crc=fields
                require((f,b)==(first,blocks), f'read line {index} differs from the input plan')
                timing={}
            else:
                _,elapsed,command,first_byte,done,clean,invalidate,polls,crc=fields
                saturated=any(x==limit for x,limit in zip(fields[1:8],(0xfffff,0xffff,0xfffff,0xfffff,0xfff,0xfff,0xffff)))
                require(elapsed>0 and polls>0 and (clean<=command or command==0xffff) and
                        (command<=first_byte or first_byte==0xfffff) and
                        (first_byte<=done or done==0xfffff) and (done<=elapsed or elapsed==0xfffff) and
                        (clean+invalidate<=elapsed or elapsed==0xfffff), 'invalid DMA timing order')
                timing=dict(command_us=command,first_byte_us=first_byte,done_us=done,clean_us=clean,
                            invalidate_us=invalidate,polls=polls,timing_saturated=saturated)
            value['reads'].append(dict(first=first,blocks=blocks,bytes=512*blocks,elapsed_us=elapsed,crc32=crc,**timing))
        elif line.startswith('C '):
            match = re.fullmatch(r'C ([0-9a-f]{2}) ([0-9a-f]{8})', line)
            require(given['version'] == VERSION and given['profile'] == 2 and plan == CHAIN_READ_PLAN and
                    match is not None, 'unexpected chain CRC line')
            index, crc = (int(x, 16) for x in match.groups())
            require(index == len(chunks) and index < 16, 'chain CRC index')
            chunks.append(crc)
        elif line.startswith('CARD '):
            require(value['card'] is None,'duplicate card line')
            match = CARD.fullmatch(line); require(match is not None, 'malformed card line')
            g = match.groups()
            mode, hz, uhs = int(g[6], 16), int(g[7], 16), int(g[8], 16)
            value['card'] = {'cid': g[0], 'csd': g[1], 'ocr': int(g[2], 16), 'rca': int(g[3], 16),
                             'sectors': int(g[4], 16), 'scr': g[5], 'profile': mode & 255, 'bus_width': (mode >> 8) & 255,
                             'timing': (mode >> 16) & 255, 'divisor_word': mode >> 24, 'card_hz': hz,
                             'uhs_outcome': uhs & 15, 'signal_1v8': bool(uhs >> 4 & 1), 'uhs_caps': uhs >> 5 & 7,
                             'sample_delay': uhs >> 8 & 255, 'tuning_start': uhs >> 16 & 255, 'tuning_width': uhs >> 24,
                             'tuning_map': g[9], 'identified_us': int(g[10], 16)}
            require(value['card']['profile'] == given['profile'], 'card line names another profile')
        elif line.startswith('D') and len(line) == 131 and line[2] == ' ':
            value['first_sector_hex'] = (value['first_sector_hex'] or '') + line[3:]
        elif line.startswith('MBUS ') and value.get('mbus') is None:
            before, after, code = line.split()[1:4]
            value['mbus'] = {'before': int(before, 16), 'after': int(after, 16), 'result': int(code, 16)}
        elif line == COMPLETE:
            require(value['outcome'] is None,'duplicate terminal line')
            value['outcome'] = 'complete'
        elif line.startswith(INCOMPLETE):
            require(value['outcome'] is None,'duplicate terminal line')
            value['outcome'] = line[len(INCOMPLETE):]
    value['words'] = words
    if given['version'] == VERSION and given['profile'] == 2 and plan == CHAIN_READ_PLAN:
        value['chunk_crc32'] = chunks
    require(words.get('SD_READS') == len(value['reads']) and words.get('SD_PLAN') == len(plan), 'read count lines')
    if value['first_sector_hex'] is not None:
        require(len(value['first_sector_hex']) == 1024 and value['reads'], 'first sector dump')
    if value['outcome'] == 'stopped':
        reason, stage, protocol = words.get('SD_REASON'), words.get('SD_STAGE'), words.get('SD_CORE_PROTOCOL')
        value['stop'] = {'reason': reason, 'reason_text': REASONS.get(reason),
                         'stage': STAGES[stage] if isinstance(stage, int) and stage < len(STAGES) else stage,
                         'core_protocol': PROTOCOLS[protocol] if isinstance(protocol, int) and protocol < len(PROTOCOLS)
                         else protocol, 'clock_off': words.get('SD_CLOCK_OFF')}
    value['complete'] = value['outcome'] == 'complete' and len(value['reads']) == len(plan) and status in (0, None)
    # Earlier frozen images have no return-version marker. Preserve their
    # decoding, while a new image cannot publish success without its EL3 receipt.
    if 'SD_RETURN_VERSION' in words:
        require(words['SD_RETURN_VERSION']==1, 'unsupported return version')
        if value['complete']:
            require(status==0, 'missing successful return status')
            require(given['profile']==2 and words.get('SD_RESTORED')==1, 'missing SD restoration receipt')
    if value['complete']:
        require(value['card'] is not None, 'missing card line')
        if plan == CHAIN_READ_PLAN:
            require(len(chunks) == 16, 'incomplete chain CRC coverage')
        value['throughput'] = (throughput().throughput_summary(value['reads']) if given['version']==1 else
                               dma_summary(value['reads'],value['card']['card_hz']) if plan==dma_plan() else {})
    return value


def decode(run, raw_input):
    session = json.loads((Path(run) / 'session.json').read_text())
    status = (session.get('result') or {}).get('status')
    log = (Path(run) / 'kernel.log').read_text()
    value = decode_log(log, raw_input, status)
    value.update(run=str(Path(run).resolve()), image_sha256=session.get('image_sha256'),
                 returned_node=session.get('returned_node'), physical_access=True)
    return value



def validate_successful_return(value, terminal, raw_input, reference):
    """Normal 3.3 V continuation only; caller must bind the exact frozen image.

    The card stays powered in high-speed transfer state. Another SD operation
    requires a separate authorized CMD0 and complete provider admission.
    """
    given = parse_input(raw_input)
    require(given['version'] == VERSION and given['profile'] == 2 and
            given['plan'] in (FIRST_READ_PLAN, CHAIN_READ_PLAN),
            'restored return requires a profile-2 finite read input')
    require(terminal.get('magic') == 0x534b4552 and terminal.get('stage') == 5 and terminal.get('status') == 0
            and value.get('complete') is True and value.get('status') == 0, 'unsuccessful terminal return')
    words = value['words']
    require(words.get('SD_PROFILE') == given['profile'] and words.get('SD_GENERATION') == given['generation'],
            'return identity differs from input')
    require(all(words.get(name) == 1 for name in
                ('SD_RETURN_VERSION', 'SD_READS', 'SD_PLAN', 'SD_PMIC_PARKED', 'SD_CLOCK_OFF', 'SD_RESTORED')),
            'incomplete restored-return receipts')
    card = value['card']
    require(card['profile'] == 2 and card['bus_width'] == 4 and card['timing'] == 1 and
            card['card_hz'] == 50000000 and not card['signal_1v8'] and card['uhs_outcome'] == 0,
            'return card mode is not 3.3 V high speed')
    chain = given['plan'] == CHAIN_READ_PLAN
    comparison = compare_chain_reference(value, reference) if chain else compare_reference(value, reference, True)
    return dict(policy=CHAIN_RETURN if chain else RESTORED_RETURN, normal_successor_eligible=True,
                input_sha256=hashlib.sha256(raw_input).hexdigest(), reference_crc=comparison,
                card_remains_powered=True, another_sd_operation_requires_new_admission=True,
                fault_recovery_qualified=False)


def capture_continuity(provenance, session):
    expected = provenance.get('warm_return_sha256', [])
    links = (session.get('admission') or {}).get('warm_returns', [])
    if not expected and not links: return {}
    require(isinstance(expected, list) and expected and
            all(isinstance(h, str) and re.fullmatch('[0-9a-f]{64}', h) for h in expected) and
            isinstance(links, list) and all(isinstance(x, dict) for x in links) and
            [x.get('sha256') for x in links] == expected,
            'capture differs from signed warm-return chain')
    admission = session['admission']
    require(all(isinstance(provenance.get(k), str) and re.fullmatch('[0-9a-f]{64}', provenance[k])
                for k in ('ddr_session_sha256', 'survey_session_sha256')) and
            admission.get('ddr_sha256') == provenance.get('ddr_session_sha256') and
            admission.get('survey_sha256') == provenance.get('survey_session_sha256') and
            session.get('initial_node') == provenance.get('effective_node') and
            session.get('initial_identity') == provenance.get('effective_identity'),
            'capture differs from effective warm-return identity')
    return {k: provenance[k] for k in ('warm_return_sha256', 'effective_node', 'effective_identity')}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='operation', required=True)
    s = sub.add_parser('sign')
    for name in ('approval', 'ddr', 'survey'): s.add_argument('--' + name, type=Path, required=True)
    s.add_argument('--signer', required=True)
    s.add_argument('--return-policy', choices=RETURN_POLICIES, default=POWER_REMOVAL)
    s.add_argument('--request', type=Path, help='required for restored-return signing')
    s.add_argument('--warm-return', type=Path, action='append', default=[],
                   help='accepted restored entry checkpoint session.json; repeat in execution order')
    i = sub.add_parser('input')
    for name in ('approval', 'request', 'ddr', 'survey', 'fabric_read', 'config', 'output'):
        i.add_argument('--' + name.replace('_', '-'), type=Path, required=True)
    i.add_argument('--profile', type=int, choices=(2, 3), default=2)
    i.add_argument('--plan', choices=('measurement', 'first-read', 'chain-read'), default='measurement',
                   help='measurement: 240 reads; first-read: 4 KiB; chain-read: 64 KiB; finite plans require profile 2')
    i.add_argument('--return-policy', choices=RETURN_POLICIES, default=POWER_REMOVAL)
    i.add_argument('--warm-return', type=Path, action='append', default=[])
    d = sub.add_parser('decode')
    for name in ('run', 'input', 'output'): d.add_argument('--' + name, type=Path, required=True)
    d.add_argument('--reference', type=Path, help='decoded accepted PIO result for the same card')
    d.add_argument('--require-restored-return', action='store_true',
                   help='require exact finite-plan normal return and reference before successor admission')
    args = parser.parse_args(argv)
    if args.operation == 'sign':
        print(json.dumps(sign(args.approval, args.ddr, args.survey, args.signer, args.return_policy,
                              args.request, args.warm_return), indent=1)); return 0
    if args.operation == 'input':
        require(args.plan != 'first-read' or args.profile == 2, 'first-read requires profile 2')
        require(args.plan != 'chain-read' or args.profile == 2, 'chain-read requires profile 2')
        require(args.return_policy != RESTORED_RETURN or (args.profile == 2 and args.plan == 'first-read'),
                'restored return requires --profile 2 --plan first-read')
        require(args.return_policy != CHAIN_RETURN or (args.profile == 2 and args.plan == 'chain-read'),
                'chain return requires --profile 2 --plan chain-read')
        lifetime(args.ddr, args.survey, args.warm_return)
        if args.warm_return:
            approved(args.approval, args.request, args.ddr, args.survey, BUNDLES[args.profile],
                     args.return_policy, args.warm_return)
        elif args.return_policy == POWER_REMOVAL:
            approved(args.approval, args.request, args.ddr, args.survey, BUNDLES[args.profile])
        else:
            approved(args.approval, args.request, args.ddr, args.survey, BUNDLES[args.profile], args.return_policy)
        require(not args.output.exists(), 'output exists')
        config = json.loads(args.config.read_text())
        require('fabric' not in config and 'fabric_hz' not in config, 'the fabric words come from this lifetime\'s read')
        (config['fabric'], config['fabric_hz']), registers = (fabric_read(args.fabric_read, args.ddr, args.warm_return)
            if args.warm_return else fabric_read(args.fabric_read, args.ddr))
        raw = input_bytes(args.profile, secrets.randbits(63) | 1, config,
                          FIRST_READ_PLAN if args.plan == 'first-read' else
                          CHAIN_READ_PLAN if args.plan == 'chain-read' else dma_plan())
        args.output.write_bytes(raw)
        args.output.with_suffix('.json').write_text(json.dumps({
            'input_sha256': hashlib.sha256(raw).hexdigest(), 'profile': args.profile,
            'return_policy': args.return_policy, 'request_sha256': digest(args.request),
            'approval_sha256': digest(args.approval), 'config_sha256': digest(args.config),
            'fabric_registers': [f'0x{w:08x}' for w in registers], 'fabric': [f'0x{w:08x}' for w in config['fabric']],
            'fabric_hz': config['fabric_hz'], 'fabric_read_sha256': digest(args.fabric_read / 'session.json'),
            'ddr_session_sha256': digest(args.ddr / 'session.json'),
            'survey_session_sha256': digest(args.survey / 'session.json'), 'tool_sha256': digest(__file__),
            **warm_binding(args.warm_return, args.survey)}, indent=1) + '\n')
        print(args.output); return 0
    raw = args.input.read_bytes(); given = parse_input(raw)
    first_read = given['version'] == VERSION and given['profile'] == 2 and given['plan'] == FIRST_READ_PLAN
    chain_read = given['version'] == VERSION and given['profile'] == 2 and given['plan'] == CHAIN_READ_PLAN
    value = decode(args.run, raw)
    require(not (first_read and value['complete']) or args.reference is not None,
            'successful first-read requires --reference')
    require(not (chain_read and value['complete']) or args.reference is not None,
            'successful chain-read requires --reference')
    if args.reference and not args.require_restored_return:
        reference = json.loads(args.reference.read_text())
        value['reference_crc'] = (compare_chain_reference(value, reference) if chain_read else
                                  compare_reference(value, reference, first_read))
        value['reference_sha256'] = digest(args.reference)
    if args.require_restored_return:
        require(args.reference is not None, 'restored return requires --reference')
        provenance = json.loads(args.input.with_suffix('.json').read_text())
        require((first_read or chain_read) and
                provenance.get('return_policy') == (CHAIN_RETURN if chain_read else RESTORED_RETURN) and
                provenance.get('input_sha256') == hashlib.sha256(raw).hexdigest(),
                'input provenance does not authorize restored return')
        for name in ('approval_sha256', 'request_sha256'):
            require(isinstance(provenance.get(name), str) and re.fullmatch('[0-9a-f]{64}', provenance[name]),
                    'input provenance lacks approval/request identity')
        session = json.loads((args.run / 'session.json').read_text())
        require(session.get('complete') is True and session.get('launch_attempted') is True and
                session.get('transport_failed') is False and session.get('automatic_access') is True and
                session.get('mode') == 'kernel' and isinstance(session.get('returned_node'), str),
                'runner did not establish successful ROM return')
        continuity = capture_continuity(provenance, session)
        value['successful_return'] = validate_successful_return(value, session.get('result') or {}, raw,
                                                               json.loads(args.reference.read_text()))
        value['reference_crc'] = value['successful_return']['reference_crc']
        value['reference_sha256'] = digest(args.reference)
        value['successful_return'].update(reference_sha256=value['reference_sha256'],
            approval_sha256=provenance['approval_sha256'], request_sha256=provenance['request_sha256'], **continuity)
    args.output.write_text(json.dumps(value, indent=1) + '\n')
    print(json.dumps({k: value[k] for k in ('status', 'outcome', 'complete')} |
                     ({'throughput': value['throughput']} if value.get('throughput') else {}) |
                     ({'stop': value['stop']} if value.get('stop') else {}), indent=1))
    # A decoded run exits 0 whatever its outcome; a log that does not decode exits nonzero.
    return 0


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, ValueError, KeyError) as error: sys.exit(f'Stopped: {error}')
