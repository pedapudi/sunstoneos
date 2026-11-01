"""Prepare finite NPU development sessions; observations remain source-labelled.

This adapter uses the NPU wire layout and finite return parser. A template
supplies PMIC/pin authority, not proof of physical qualification. Live pin
readbacks supply the inherited electrical fields that native guards verify.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import secrets
import struct

ROOT = Path(__file__).resolve().parents[2]
_mbus_spec = importlib.util.spec_from_file_location('npu_vendor_mbus', ROOT / 'tools/vendor-mbus.py')
_mbus = importlib.util.module_from_spec(_mbus_spec)
_mbus_spec.loader.exec_module(_mbus)
# Accessible system/controller registers only; never the NPU engine window.
WORDS = (0x0300624c, 0x03000008, 0x02002500, 0x02002580, 0x02002588,
         0x02002b00, 0x02002b04, 0x020022a0, 0x020025c0, 0x020025e0,
         0x02003920, 0x020039a0, 0x020022a4, 0x020022a8, 0x020022ac, 0x020022b0,
         0x070101ac, 0x07025000, 0x07025014, 0x07025024)
MASKS = (0x0300001f, 0x8700001f, 0x8700001f)
PARENTS = ({0: 24, 3: 600}, {0: 24, 2: 800, 3: 600, 4: 480},
           {0: 24, 1: 600, 3: 480, 4: 400})


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def initial_state(require, words):
    """Readable preconditions from a733_npu_startup.c pass(); never open the engine."""
    def word(address):
        value = words.get(hex(address))
        require(type(value) is int and 0 <= value <= 0xffffffff, 'Missing NPU system readback: ' + hex(address))
        return value
    require(word(0x03000008) & 2, 'Shared SRAM is not routed to the CPU.')
    module = word(0x02002b00)
    require(not module & 0x80000000 and ((module >> 24) & 7) <= 6 and
            word(0x02002b04) & 0xf0001 == 0x80000, 'NPU local gates/resets are not closed.')
    # Native NPU_FABRIC ownership enables these gates before PCK accesses.
    # Their entry values are diagnostics, not pre-existing ownership.
    for address in (0x020025c0, 0x020025e0, 0x070101ac):
        word(address)
    pll = word(0x020022a0)
    without_inherited = pll & ~(1 << 24)
    adopted = without_inherited in (0xf8104600, 0xf8305100)
    if adopted:
        require(word(0x02003920) & 0x10001 == 0x10001 and word(0x020039a0) & 0x10000,
                'Inherited NPU PLL gate state differs.')
    else:
        require(not pll & 0xb8000000 and not pll & ~0xf970ff02,
                'NPU PLL is neither disabled nor an admitted locked operating point.')
    require(not word(0x020022a4) & 1 and not word(0x020022a8) & 0x80000000 and
            word(0x020022ac) & 0xff100000 == 0x30000000 and
            word(0x020022b0) & 0x1f0000 == 0xb0000, 'NPU PLL tuning or pattern state differs.')


def session(finite, image_data, layout, template, live):
    """Rebind identities and live PMIC pin settings; retain template authority."""
    require = finite.require
    size, boot, boot_bytes, npu, pmic, pins = layout
    require(tuple(layout) == (456, 224, 232, 72, 96, 128), 'NPU development layout differs.')
    require(len(template) == size, 'NPU --records must be a 456-byte native session template.')
    finite.original.session_profile(template, layout)
    require(struct.unpack_from('<3Q', template) == (finite.original.SESSION_MAGIC, 3, size),
            'NPU session template version differs.')
    words = live['words']
    initial_state(require, words)
    sid = words[hex(0x0300624c)]
    code = (sid >> 16) & 255 or (sid >> 8) & 255
    require(code == 4, 'NPU development requires bin 4: vendor top 852 MHz at 800 mV.')
    pin_words = []
    for address in (0x07025000, 0x07025014, 0x07025024):
        value = words.get(hex(address))
        require(type(value) is int and 0 <= value <= 0xffffffff,
                'Missing NPU PMIC pin readback: ' + hex(address))
        pin_words.append(value)
    pin_configs = []
    for pin in range(2):
        function, drive, pull = ((pin_words[0] >> (4 * pin)) & 15,
                                 (pin_words[1] >> (4 * pin)) & 15,
                                 (pin_words[2] >> (2 * pin)) & 3)
        require(function == 2, 'NPU PMIC pins are not inherited R-TWI.')
        require(drive <= 3 and pull <= 2, 'Unsupported inherited NPU PMIC pin electrical fields.')
        pin_configs.append({'pin': pin, 'function': function, 'pull': pull, 'drive': drive})
    values, rates = [], []
    for index, address in enumerate((0x02002500, 0x02002580, 0x02002588)):
        require(not index or not words[hex(address)] & 0x18000000, 'NPU fabric clock has unsupported control bits.')
        value = words[hex(address)] & MASKS[index]
        parent = PARENTS[index].get((value >> 24) & 7, 0) * 1000000
        divisor = (value & 31) + 1
        require(parent and parent % divisor == 0 and (not index or value & (1 << 31)),
                'Unsupported NPU fabric clock.')
        values.append(value)
        rates.append(parent // divisor)
    before_values, before_rates = list(values), list(rates)
    # EL3 authenticates/seals this record before restoring ROM's 400 MHz
    # MBUS to boot0's 600 MHz. Name that post-entry state now; the driver's
    # exact comparison still refuses if the restore skips or hardware differs.
    values, rates = _mbus.fabric(values, rates)
    raw = bytearray(template)
    epoch, generation, lease = (secrets.randbits(62) | 1 for _ in range(3))
    struct.pack_into('<3Q', raw, 24, epoch, generation, lease)
    build = finite.original.image_data(image_data)[2]
    raw[48:80] = build
    struct.pack_into('<6I', raw, boot + npu, *values, *rates)
    # Expected and desired are identical in the native PMIC pin acquisition.
    # Only its two electrical configurations change; the pin authorization,
    # owner identities, IRQ authority and output-latch handoff stay intact.
    for config in pin_configs:
        struct.pack_into('<3B', raw, boot + pmic + 24 + 3 * config['pin'],
                         config['function'], config['pull'], config['drive'])
    derived = {'fabric': values, 'rate_hz': rates, 'mhz': 852, 'supply_uv': 800000,
               'fabric_before_entry': before_values, 'rate_hz_before_entry': before_rates,
               'mbus_rule_sha256': sha((ROOT / 'tools/vendor-mbus.py').read_bytes()),
               'pmic_pins': pin_configs,
               'configuration_template_sha256': sha(template),
               'physical_qualification': False,
               'basis': 'template PMIC/pin authority, live pin/fabric/SID readbacks, expected EL3 MBUS restore and native entry guards'}
    # Bind actual source-labelled records, without fabricating review or
    # measured electrical facts from a software template.
    records = [dict(live=live, derived=derived), {'clock_readbacks': live['words'], 'derived': derived},
               {'template_pmic_pin_sha256': sha(template[boot + pmic:]), 'pmic_pins': pin_configs,
                'supply_check': 'native PMIC guard'},
               {'watchdog_mode': 0xb1, 'session_seconds': 20, 'attempts': 1,
                'normal_return': 'native quiescence, SRAM handback and watchdog disarm required'}]
    for at, record in zip((80, 112, 144, 176), records):
        raw[at:at + 32] = hashlib.sha256(json.dumps(record, sort_keys=True).encode()).digest()
    profile = finite.original.session_profile(bytes(raw), layout)
    return bytes(raw), profile, derived, records


def prepare(finite, image, manifest, template, live, output, clang, lld, objcopy):
    data = image.read_bytes()
    parts, address, build, layout, geometry = finite.image_profile(data, json.loads(manifest.read_bytes()))
    raw, profile, derived, records = session(finite, data, layout, template.read_bytes(), live)
    output.mkdir(parents=True, exist_ok=False)
    (output / 'image.elf').write_bytes(data)
    (output / 'manifest.json').write_bytes(manifest.read_bytes())
    (output / 'session.bin').write_bytes(raw)
    finite.build_launcher(output, clang, lld, objcopy, watchdog_mode=0xb1)
    for name, value in zip(('request.bin', 'contract.bin'), finite.original.request_bytes(address, build, raw, profile)):
        (output / name).write_bytes(value)
    (output / 'result-zero.bin').write_bytes(bytes(16384))
    (output / 'report-zero.bin').write_bytes(bytes(128))
    segments = []
    for index, part in enumerate(parts):
        if part['data']:
            name = f'segment-{index}.bin'
            (output / name).write_bytes(part['data'])
            segments.append({'name': name, 'address': part['address']})
    segments.append({'name': 'session.bin', 'address': 0x41f00000})
    names = ['image.elf', 'manifest.json', 'launch.bin', 'session.bin', 'request.bin', 'contract.bin',
             'result-zero.bin', 'report-zero.bin'] + [i['name'] for i in segments[:-1]]
    metadata = {'schema': 'sunstoneos-npu-development-v1', 'image_sha256': sha(data), 'build_id': build.hex(),
                'execution_profile': finite.geometry_profile(geometry), 'geometry': geometry,
                'session_layout': list(layout), 'watchdog': {'mode': 0xb1, 'session_seconds': 20},
                'segments': segments, 'derived_session': derived, 'records': records, 'live': live,
                'files': {name: sha((output / name).read_bytes()) for name in names}, **profile}
    finite.save(output / 'dev-bundle.json', metadata)
    return metadata


def validator(finite, marker_reader):
    def validate(bundle):
        bundle = Path(bundle)
        p = json.loads((bundle / 'dev-bundle.json').read_bytes())
        finite.require(p.get('schema') == 'sunstoneos-npu-development-v1', 'Wrong NPU development bundle.')
        frozen = {name: (bundle / name).read_bytes() for name in p['files']}
        finite.require(all(sha(raw) == p['files'][name] for name, raw in frozen.items()), 'NPU bundle changed.')
        finite.require(p['watchdog'] == marker_reader(frozen['image.elf']) == {'mode': 0xb1, 'session_seconds': 20},
                       'NPU watchdog marker differs.')
        parts, address, build, layout, geometry = finite.image_profile(frozen['image.elf'], json.loads(frozen['manifest.json']))
        raw = frozen['session.bin']
        profile = finite.original.session_profile(raw, layout)
        finite.require(p['image_sha256'] == sha(frozen['image.elf']) and
                       p['execution_profile'] == finite.geometry_profile(geometry) and p['session_layout'] == list(layout) and
                       p['geometry'] == geometry and p['build_id'] == build.hex() and raw[48:80] == build and
                       all(p[k] == v for k, v in profile.items()), 'NPU image/session identity differs.')
        expected = []
        for index, part in enumerate(parts):
            if part['data']:
                name = f'segment-{index}.bin'
                finite.require(frozen[name] == part['data'], 'NPU ELF segment differs.')
                expected.append({'name': name, 'address': part['address']})
        expected.append({'name': 'session.bin', 'address': 0x41f00000})
        finite.require(p['segments'] == expected, 'NPU upload geometry differs.')
        for at, record in zip((80, 112, 144, 176), p['records']):
            finite.require(raw[at:at + 32] == hashlib.sha256(json.dumps(record, sort_keys=True).encode()).digest(),
                           'NPU development record differs from session.')
        finite.require(len(p['records']) == 4 and p['records'][0]['live'] == p['live'],
                       'NPU development state binding differs.')
        request, contract = finite.original.request_bytes(address, build, raw, profile)
        finite.require(frozen['request.bin'] == request and frozen['contract.bin'] == contract and
                       frozen['result-zero.bin'] == bytes(16384) and frozen['report-zero.bin'] == bytes(128),
                       'NPU launch packet differs.')
        p.update(_frozen=frozen, _sizes={name: len(raw) for name, raw in frozen.items()})
        return p
    return validate
