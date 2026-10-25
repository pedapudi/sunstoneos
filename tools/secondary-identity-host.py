"""Frozen bindings and result validation for the sequential secondary survey."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from types import SimpleNamespace
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('identity_builder', ROOT/'tools/prepare-secondary-identity-probe.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
primary = builder.primary
require, digest = primary.require, primary.digest
PROFILE = 'secondary-identity-return-v1'
HOST_SOURCES = primary.HOST_SOURCES + ('tools/secondary-identity-host.py',)
MODEL_SOURCES = ('checks/secondary-identity-probe.py',) + primary.MODEL_SOURCES
FIELDS = ('epoch','core','midr','mpidr','sctlr3','sctlr1','tcr1','clusteridr','clustercfr',
          'clidr','mmfr0','power_before','power_after','target_control','target_mode',
          'ready','request','ack','architecture','entry_low','entry_high','control','mode',
          'off','restored','owner_state','report_state')


def decode(raw, nonce, local=False, cached=False, coherent=False, interrupts=False, pre_scheduler=False):
    coherent = coherent or interrupts or pre_scheduler
    cached = cached or coherent
    local = local or cached
    require(len(raw) == (0x5200 if pre_scheduler else 0x4200 if coherent else 0x3000 if cached else 0x1c00 if local else 0xa00), 'Short combined CPU report.')
    result = primary.probe.decode(raw[:256], nonce)
    header = struct.unpack_from('<32Q', raw, 256)
    require(header[:3] == ((0x4137335052455331,1,0x5100) if pre_scheduler else (0x4137334952515331,1,0x4100) if interrupts else (0x413733434f484531,1,0x4100) if coherent else (0x4137334341434831,1,0x2f00) if cached else (0x4137334c4f434131,1,0x1b00) if local else (0x4137334944454e31,1,0x900)) and header[3] and header[4] and
            header[5:8] == ((0xfe,2,7) if local else (0xfc,2,7)) and
            (header[8:12] == tuple(result['el1_before'])*2 and not any(header[12:])
             if pre_scheduler else not any(header[8:])), 'Incomplete secondary survey.')
    require(header[4] > result['counter_after'], 'Survey deadline preceded primary preparation.')
    require(not any(raw[512:768 if local else 1024]), 'Untested CPU slots changed.')
    cores = []
    for core in range(1 if local else 2, 8):
        values = struct.unpack_from('<32Q', raw, 512+core*256)
        s = dict(zip(FIELDS, values))
        require(s['epoch'] == s['request'] == s['ack'] == header[3] and s['core'] == core and
                s['ready'] == s['restored'] == 1 and s['owner_state'] == s['report_state'] == 2 and
                not s['off'] & 15 and (values[27:] == (1,0,0,0,0) if coherent else not any(values[27:])), 'Core entry/shutdown/restoration incomplete.')
        require(s['midr'] & 0xffffffffff0ffff0 == (0x410fd050 if core < 6 else 0x410fd0b0) and
                not s['mpidr'] & ~0x000000ffc1ffffff and
                not s['mpidr'] & 0x000000ff00ff00ff and (s['mpidr'] >> 8) & 255 == core and
                not s['sctlr3'] & 0x02001005, 'Core identity or entry controls differ.')
        require(s['target_control'] == s['control'] and s['target_mode'] == s['mode'] and
                not (s['control'] | s['mode'] | s['power_before']) & 1 and
                s['power_after'] == s['power_before'] | 1, 'Core shutdown controls differ.')
        cores.append(s)
    result.update(epoch=header[3], startup_deadline=header[4], completed_mask=header[5],
                  secondary_entry_and_off_completed=True, cores=cores)
    return result


probe = SimpleNamespace(NATIVE=primary.probe.NATIVE, REPORT=(primary.probe.REPORT[0],0xa00), decode=decode)


def initial_report(nonce):
    return primary.initial_report(nonce)+bytes(0x900)


def load(build, image, manifest, local=False, cached=False, coherent=False, interrupts=False, pre_scheduler=False, first_task=False, task_switch=False, task_workload=False,
         return_deadline=False, quiet=False):
    """Check a build against the frozen inputs: the builder's patch plan,
    reapplied to the frozen base overlay and native segments, must reproduce
    every built byte and the recorded patch inventory."""
    f = builder.profile(local=local, cached=cached, coherent=coherent, interrupts=interrupts,
                        pre_scheduler=pre_scheduler, first_task=first_task, task_switch=task_switch,
                        task_workload=task_workload, return_deadline=return_deadline, quiet=quiet)
    report = builder.report_area(f)
    require(manifest.get('schema') == builder.schema(f) and
            manifest.get('physical_access') is False and manifest.get('native_sha256') == probe.NATIVE,
            'Wrong secondary identity profile.')
    require(manifest['sources'] == {n:digest(ROOT/n) for n in builder.sources(f)}, 'Identity source changed.')
    base_path = Path(manifest['primary_manifest']['path'])
    require(base_path.is_absolute() and digest(base_path) == manifest['primary_manifest']['sha256'],
            'Base primary manifest changed.')
    base = json.loads(base_path.read_text())
    frozen, dram = primary.load(base_path.parent, image, base)
    require(manifest['identity_report'] == {'address':report[0],'bytes':report[1]} and
            manifest['reservations'] == [list(v) for v in dram+[('identity-report',*report)]],
            'Identity report or backup reservations changed.')
    require(all(report[0]+report[1] <= a or a+n <= report[0] for _,a,n in dram),
            'Report overlaps existing storage.')
    with (build/'identity.elf').open('rb') as stream:
        hooks = {s.name:s['st_value'] for s in ELFFile(stream).get_section_by_name('.symtab').iter_symbols()
                 if s.name.startswith('identity_') or (f.cached and s.name.startswith(('native_el','el1_')))}
    require(hooks == manifest['hooks'], 'Identity hooks differ.')
    with image.open('rb') as stream:
        symbols = list(ELFFile(stream).get_section_by_name('.symtab').iter_symbols())
        for name, address in manifest['native_symbols'].items():
            require([s['st_value'] for s in symbols if s.name == name and s['st_shndx'] != 'SHN_UNDEF'] == [address],
                    'Native symbol differs: '+name)
    for name in ('identity.elf','identity.bin'):
        require(digest(build/name) == manifest['files'][name], 'Identity executable changed.')
    address = base['hooks']['primary_publication_return']
    require(hooks['identity_original_return_next'] == address+4, 'Original return continuation differs.')
    overlay = bytearray(frozen['overlay.bin'])
    offset = address-primary.probe.OVERLAY[0]
    original = struct.unpack_from('<I',overlay,offset)[0]
    replacement = builder.branch(address,hooks['identity_primary_gate'])
    patches = [{'address':address,'original':original,'replacement':replacement,'space':'overlay'}]
    struct.pack_into('<I',overlay,offset,replacement)
    overlay += bytes(builder.extension_address(f)-primary.probe.OVERLAY[0]-len(overlay))
    overlay += (build/'identity.bin').read_bytes()
    require(bytes(overlay) == (build/'overlay.bin').read_bytes() and len(overlay) <= primary.probe.OVERLAY[1],
            'Composite overlay differs beyond reviewed extension.')
    require(manifest['overlay'] == dict(base['overlay'],sha256=hashlib.sha256(overlay).hexdigest()),
            'Overlay reservation or hash changed.')
    frozen['overlay.bin'] = bytes(overlay)
    plan = builder.patch_plan(manifest['native_symbols'], hooks, f)
    patched, records = builder.apply_plan(plan, base['payloads'], frozen)
    payloads = []
    for item in base['payloads']:
        data = patched[item['name']]
        require(data == (build/item['name']).read_bytes(), 'Native payload has an unreviewed change.')
        frozen[item['name']] = data
        payloads.append(dict(item,sha256=hashlib.sha256(data).hexdigest()))
    require(manifest['patches'] == patches+records and manifest['payloads'] == payloads,
            'Patch or payload inventory differs.')
    return frozen, dram+[('identity-report',*report)]


def validate_software(software, manifest_hash):
    require(software.get('schema') == 'a733-secondary-identity-software-v1' and
            software.get('physical_access') is False and software.get('manifest_sha256') == manifest_hash and
            software.get('host_sources') == {n:digest(ROOT/n) for n in HOST_SOURCES}, 'Host evidence inputs differ.')
    require(software.get('host_cases') == dict.fromkeys(('complete-return','transport-failure-retained',
            'invalid-report-restored','changed-input-refused','incomplete-shutdown-refused'),True),
            'Host cases are incomplete.')
    item = software['instruction_evidence']
    path = Path(item['path'])
    require(path.is_absolute() and digest(path) == item['sha256'], 'Instruction evidence changed.')
    result = json.loads(path.read_text())
    require(result.get('physical_access') is False and result.get('native_sha256') == probe.NATIVE and
            result.get('manifest_sha256') == manifest_hash and
            result.get('sources') == {n:digest(ROOT/n) for n in MODEL_SOURCES}, 'Instruction inputs differ.')
    cases = {c['case']:c for c in result['cases']}
    require(set(cases) == {'sequential-six-core-entry-off-restore-return','ack-without-off','restore-readback'} and
            all(c.get('pass') is True for c in cases.values()), 'Instruction cases are incomplete.')
