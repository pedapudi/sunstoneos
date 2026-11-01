"""Exact final-retirement review and copied report for returning GPU recognition."""
import struct

CONDITIONS = {'output-lending-retired', 'firmware-off-isolation-and-resources',
              'gpu-worker-and-pmic-sample-parked', 'spi95-restoration',
              'local-pools-and-read-only-cache-retirement'}
MAGIC, BYTES = 0x3152475033333741, 64
CONSTRUCTOR_STAGES = (
    None, 'previous GPU-loss record', 'owner identities', 'tensor table',
    'native platform constructor', 'platform constructor', 'PMIC lifetime',
    'PMIC binding', 'performance selection', 'firmware memory', 'mimus memory',
    'native backend binding', 'service construction', 'recognizer client binding',
    'thermal client binding')
BISECT_STAGES = (None, 'constructor', 'firmware', 'jobs', 'layer')


def validate(review, config, evidence, require):
    value = review.get('gpu_retirement')
    require(isinstance(value, dict) and value.get('configuration') == config and
            isinstance(value.get('conditions'), dict) and set(value['conditions']) == CONDITIONS,
            'Exact GPU final-retirement review required.')
    for item in value['conditions'].values():
        evidence(item)


def decode(raw, epoch, generation, require, allow_untouched=False):
    require(len(raw) == BYTES, 'Incomplete GPU retirement report.')
    magic, version, size, actual, gen, jobs, flags, uv = struct.unpack('<8Q', raw)
    if version == 2:
        require(allow_untouched and (magic, size, actual, gen, jobs, flags) ==
                (MAGIC, BYTES, epoch, generation, 0, 0) and 1 <= uv <= 14,
                'GPU untouched refusal is incomplete or not admitted by this image.')
        return {'jobs': 0, 'gpu_untouched': True, 'constructor_refusal': uv,
                'constructor_stage': CONSTRUCTOR_STAGES[uv],
                'qualification_passed': False}
    if version == 3:
        # A bisect image (SUNSTONE_A733_OS_GPU_BISECT) stopped GPU use at a
        # build-time limit. Only those images write version 3; they also carry
        # the untouched-return marker that admits version 2.
        stage, limit = uv >> 32, uv & 0xffffffff
        require(allow_untouched and (magic, size, actual, gen) == (MAGIC, BYTES, epoch, generation) and
                1 <= stage <= 4 and flags == (0 if stage == 1 else 63) and
                (stage != 1 or jobs == 0) and jobs < 0xffffffff,
                'GPU bisect retirement is incomplete or belongs to another session.')
        return {'jobs': jobs, 'gpu_bisect': {'stage': BISECT_STAGES[stage], 'limit': limit},
                'gpu_off': flags == 63, 'gpu_never_powered': stage == 1,
                'worker_parked': flags == 63, 'qualification_passed': False}
    require((magic, version, size, actual, gen, flags, uv) ==
            (MAGIC, 1, BYTES, epoch, generation, 63, 960000) and 0 < jobs < 0xffffffff,
            'GPU retirement is incomplete or belongs to another session.')
    return {'jobs': jobs, 'gpu_off': True, 'worker_parked': True,
            'pmic_sample_retired': True, 'dcdc4_uv': uv}
