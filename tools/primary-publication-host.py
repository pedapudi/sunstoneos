"""Offline bindings for the returning primary probe and existing FEL runner."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('primary_preparation', ROOT/'tools/prepare-primary-publication-probe.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)
require, digest = probe.require, probe.digest
PROFILE = 'primary-publication-return-v1'
HOST_SOURCES = ('tools/run-native-startup-probe.py', 'tools/primary-publication-host.py',
                'tools/run-a64-entry-probe.py', 'tools/check-a733-smp.py',
                'tools/check-a733-smp-supervised.py', 'tools/usb-access.py')
MODEL_SOURCES = ('checks/primary-publication-probe.py', 'checks/a733-supervised-publication.py',
                 'checks/a733-smp-cached.py')


def load(build, image, manifest):
    require(manifest.get('schema') == 'a733-primary-publication-probe-v1' and
            manifest.get('physical_access') is False and manifest.get('native_sha256') == probe.NATIVE,
            'Wrong primary preparation profile.')
    require(digest(image) == probe.NATIVE, 'Primary probe image changed.')
    source_names = ('boot/a733/primary-publication-probe.S', 'boot/a733/native-startup-probe.ld',
                    'tools/prepare-primary-publication-probe.py')
    require(manifest['sources'] == {n: digest(ROOT/n) for n in source_names}, 'Probe source changed.')
    with image.open('rb') as stream:
        elf = ELFFile(stream)
        sections = [{'name': s.name, 'address': s['sh_addr'], 'bytes': s['sh_size']}
                    for s in elf.iter_sections() if s['sh_flags'] & 2 and s['sh_size']]
        symbols = {s.name: (s['st_value'], s['st_size']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
        segments = [(s['p_paddr'], s.data()) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    require(manifest['native_reservations'] == sections, 'Native reservations differ from the ELF.')
    require(manifest['layout'] == {n: symbols[n][0] for n in manifest['layout']}, 'Native layout changed.')
    require(manifest['production_prepare'] == symbols['a733_cached_prepare'][0], 'Wrong callee.')
    require(manifest['report'] == {'address': probe.REPORT[0], 'bytes': probe.REPORT[1], 'nonce_offset': 32},
            'Wrong primary report reservation.')
    overlay = manifest['overlay']
    require((overlay['address'], overlay['reservation_bytes']) == probe.OVERLAY, 'Overlay reservation changed.')
    for address, size in (probe.OVERLAY, probe.REPORT):
        require(all(address+size <= s['address'] or s['address']+s['bytes'] <= address for s in sections),
                'Overlay/report overlaps native storage.')
    frozen = {'overlay.bin': (build/'overlay.bin').read_bytes()}
    require(0 < len(frozen['overlay.bin']) <= probe.OVERLAY[1] and
            hashlib.sha256(frozen['overlay.bin']).hexdigest() == overlay['sha256'], 'Overlay bytes changed.')
    patch = manifest['patch']
    address = patch['address']
    boot, size = symbols['a733_smp_boot']
    require(boot <= address < boot+size and address % 4 == 0, 'Patch is outside native boot.')
    delta = (manifest['production_prepare']-address)//4
    require(patch['original'] == 0x94000000 | (delta & 0x3ffffff), 'Patch does not replace the production call.')
    hook = manifest['hooks']['primary_publication_probe']
    require(hook == probe.OVERLAY[0] and patch['replacement'] == 0x94000000 | (((hook-address)//4) & 0x3ffffff),
            'Patched call targets the wrong entry.')
    expected_payloads = []
    patched = 0
    for index, (start, data) in enumerate(segments):
        if not data:
            continue
        if start <= address < start+len(data):
            data = bytearray(data)
            require(struct.unpack_from('<I', data, address-start)[0] == patch['original'], 'Displaced instruction differs.')
            struct.pack_into('<I', data, address-start, patch['replacement'])
            data = bytes(data)
            patched += 1
        name = f'segment-{index}.bin'
        frozen[name] = (build/name).read_bytes()
        require(frozen[name] == data, 'Payload differs beyond the reviewed call patch.')
        expected_payloads.append({'name': name, 'address': start, 'bytes': len(data),
                                  'sha256': hashlib.sha256(data).hexdigest()})
    require(patched == 1 and manifest['payloads'] == expected_payloads, 'Payload inventory changed.')
    dram = [('native-'+s['name'].removeprefix('.'), s['address'], s['bytes']) for s in sections]
    dram += [('overlay', *probe.OVERLAY), ('primary-report', *probe.REPORT)]
    require(len({n for n, _, _ in dram}) == len(dram), 'Backup names collide.')
    return frozen, dram


def validate_software(software, manifest_hash):
    require(software.get('schema') == 'a733-primary-publication-software-v1' and
            software.get('physical_access') is False and software.get('manifest_sha256') == manifest_hash,
            'Primary host evidence covers different inputs.')
    require(software.get('host_sources') == {n: digest(ROOT/n) for n in HOST_SOURCES}, 'Host implementation changed.')
    require(software.get('host_cases') == {'complete-return': True, 'transport-failure-retained': True,
            'missing-return-retained': True, 'invalid-report-restored': True, 'changed-input-refused': True},
            'Focused host cases are incomplete.')
    item = software['instruction_evidence']
    path = Path(item['path'])
    require(path.is_absolute() and digest(path) == item['sha256'], 'Instruction evidence changed.')
    model = json.loads(path.read_text())
    require(model.get('physical_access') is False and model.get('native_sha256') == probe.NATIVE and
            model.get('manifest_sha256') == manifest_hash and
            model.get('sources') == {n: digest(ROOT/n) for n in MODEL_SOURCES}, 'Instruction inputs changed.')
    cases = {c['case']: c for c in model['cases']}
    require(set(cases) == {'actual-boot-primary-prepare-restore-return',
                          'restoration-readback-failure-retains-without-reset'} and
            all(c.get('pass') is True for c in cases.values()), 'Instruction cases are incomplete.')


def initial_report(nonce):
    require(len(nonce) == 16, 'Invalid nonce.')
    result = bytearray(probe.REPORT[1])
    result[32:48] = nonce
    return bytes(result)
