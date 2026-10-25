"""Validate all-core production startup, written pages and restored ROM return."""
import importlib.util
import json
from pathlib import Path
import struct
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('cache_host', ROOT/'tools/secondary-cache-host.py')
cache = importlib.util.module_from_spec(spec); spec.loader.exec_module(cache)
require, digest = cache.require, cache.digest
PROFILE = 'all-core-pre-scheduler-return-v1'
HOST_SOURCES = cache.HOST_SOURCES+('tools/all-core-pre-scheduler-host.py',)
MODEL_SOURCES = ('checks/all-core-pre-scheduler-probe.py',)+cache.MODEL_SOURCES
CASES = ('complete-return', 'transport-failure-retained', 'invalid-report-restored',
         'changed-input-refused', 'incomplete-primary-refused', 'incomplete-participant-refused',
         'wrong-memory-result-refused')



def expected_memory(epoch):
    page = bytearray(4096)
    for core in range(8): struct.pack_into('<2Q', page, core*64, epoch, 1)
    state = bytearray(4096)
    struct.pack_into('<19I', state, 0, *([1]*8), 256, 0, *([1]*8), 1)
    return bytes(page+state)


def decode(raw, nonce, smc_return, first_task=False, task_workload=False):
    result = cache.decode(raw, nonce, smc_return, pre_scheduler=True, first_task=first_task, task_workload=task_workload)
    require(result['cached_entry_completed'], 'Every secondary must finish cached execution.')
    observed_sctlr = 0x30d41805 if first_task else 0x30d01805
    clean_lines = 9280 if first_task else 192
    binding = (result['epoch'], result['startup_deadline'])
    local = struct.unpack_from('<64Q', raw, 0xa00)
    stable = cache.local.stable  # Ignores the snapshot words' read-only status bits.
    require(local[:3] == (3, *binding) and local[3] == 26 and not local[4] and local[5] > 0 and
            stable(local[10:20]) == stable(local[20:30]) and not local[30] and not any(local[34:]),
            'Primary GIC initialization or restoration incomplete.')
    require(not local[10] & ~cache.local.STATUS[0] and not any(local[11:15]) and not local[19] & 1 and
            ((local[19] >> 2) & 1) == ((local[19] >> 3) & 1) and
            local[7:10] == local[31:34], 'Primary GIC baseline or restored interface differs.')
    el3 = struct.unpack_from('<20Q', raw, 0x2000)
    require(el3[:4] == (3, *binding, 1) and not el3[19] and
            el3[4:8] == tuple(result['el3_before']) and not el3[4] & 0x02001005,
            'Primary cache adoption inherited different controls.')
    require(el3[9:14] == (el3[4] | 0x1005, 0x80823019, 0x421f2000, 0x44ff00, el3[8] & ~0x200) and
            el3[14:19] == el3[4:9] and not any(raw[0x20b0 if task_workload else 0x20a0:0x2100]),
            'Primary EL3 cache restoration or padding differs.')
    if task_workload:
        before, after = struct.unpack_from('<2Q', raw, 0x20a0)
        require(before == after and not before & ~0xf8f8f8f8,
                'Primary Secure timer priority was not restored.')
    el1 = struct.unpack_from('<32Q', raw, 0x2800)
    require(el1[:3] == (3, *binding) and el1[4:6] == tuple(result['el1_after']) and
            0x42000000 <= el1[3] < 0x421f0000 and not el1[3] & 15,
            'Primary EL1 entry inherited different preparation.')
    require(el1[9:23] == (0x733, 4, observed_sctlr, 0x200803019, 0x421f4000, 0x44ff00, 0,
            0x4124b000, 0, 24000000, 0x81000000, 0, 0x5e000733, smc_return) and
            el1[23] & 0x3ff == 0x3c5 and el1[29:] == (0, 8, 12) and
            el1[24:29] == el1[4:9], 'Primary EL1/SMC observation or restoration differs.')
    for core in range(8):
        require(struct.unpack_from('<8Q', raw, 0x3000+core*64) ==
                (3, core, result['epoch'], 1, 256, clean_lines, 255, 0),
                'Production pre-scheduler result or cache cleanup incomplete.')
    if not first_task:
        require(raw[0x3200:] == expected_memory(result['epoch']),
                'Physical shared memory differs from completed production startup.')
    result.update(primary_cached_entry_completed=True, primary_local_gic_restored=True,
                  production_pre_scheduler_completed=True, participants=8, atomic_increments=256,
                  cache_lines_cleaned_per_core=clean_lines, shared_memory_published_to_poc=True)
    return result


probe = SimpleNamespace(NATIVE=cache.probe.NATIVE, REPORT=(cache.probe.REPORT[0], 0x5200), decode=decode)


def initial_report(nonce): return cache.local.identity.primary.initial_report(nonce)+bytes(0x5100)


def load(build, image, manifest): return cache.local.identity.load(build, image, manifest, pre_scheduler=True)


def validate_software(software, manifest_hash):
    require(software.get('schema') == 'a733-all-core-pre-scheduler-software-v1' and
            software.get('physical_access') is False and software.get('manifest_sha256') == manifest_hash and
            software.get('host_sources') == {n: digest(ROOT/n) for n in HOST_SOURCES} and
            software.get('host_cases') == dict.fromkeys(CASES, True), 'Pre-scheduler host evidence differs.')
    item = software['instruction_evidence']; path = Path(item['path'])
    require(path.is_absolute() and digest(path) == item['sha256'], 'Instruction evidence changed.')
    result = json.loads(path.read_text())
    require(result.get('physical_access') is False and result.get('native_sha256') == probe.NATIVE and
            result.get('manifest_sha256') == manifest_hash and
            result.get('sources') == {n: digest(ROOT/n) for n in MODEL_SOURCES}, 'Instruction inputs differ.')
    require({c['case']: c.get('pass') for c in result['cases']} ==
            dict.fromkeys(('success', 'counter-corruption', 'restore-el3'), True), 'Instruction cases incomplete.')
