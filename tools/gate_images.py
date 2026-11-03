"""The images and bundles tools/integration-gate.py builds for the checks that name them in GATE_RUNS (checks/check-index.py).

IMAGES are CMake recipes built once per tree state; the gate's OS variants (VARIANTS) are named directly and need no recipe.
{elf:X} and {build:X} are an image's ELF and build directory, {session:X} the session bytes tools/gate_session.py builds for
image X, {bundle:X} the probe ELF that apps/board-bundles-power/build-opp.py builds for bundle X (BUNDLES lists its arguments).
"""

IMAGES = {
    # the CPU-return NPU image of apps/cpu-npu-concurrency (apps/cpu-npu-concurrency/README.md)
    'ret-npu-load': {'configure': [
        '-DSUNSTONE_MIMUS_BOARD_APPLICATION=ON',
        '-DSUNSTONE_APPLICATION=cpu-npu-load',
        '-DSUNSTONE_APPLICATION_cpu-npu-load_ROOT={source}/apps/cpu-npu-concurrency',
        '-DA733_A76_ADMITTED=ON',
        '-DSUNSTONE_A733_OS_CPU_CLOCK=ON',
        '-DSUNSTONE_A733_OS_CPU_RETURN=ON',
        '-DSUNSTONE_A733_OS_NPU=ON',
        '-DSUNSTONE_A733_OS_NPU_OBSERVE=ON',
        '-DSUNSTONE_OS_NPU_CATALOGUE={cache}/build/npu-board/session-artifacts.json',
        '-DSUNSTONE_OS_NPU_MODEL_NAME=shufflenet-v2-published',
    ], 'target': 'sunstone-a733-os', 'elf': 'sunstone-a733-os.elf'},
    # the CPU-return GPU image of apps/gpu-lifecycle (apps/gpu-lifecycle/README.md)
    'ret-gpu': {'configure': [
        '-DSUNSTONE_APPLICATION=gpu-lifecycle',
        '-DSUNSTONE_A733_OS_GPU=ON',
        '-DSUNSTONE_A733_OS_CPU_RETURN=ON',
        '-DSUNSTONE_A733_OS_CPU_CLOCK=ON',
        '-DA733_A76_ADMITTED=ON',
    ], 'target': 'sunstone-a733-os-gpu-lifecycle', 'elf': 'sunstone-a733-os-gpu-lifecycle.elf'},
    # the CPU-return latency diagnostic image
    'ret-latency': {'configure': [
        '-DSUNSTONE_LATENCY=ON',
        '-DSUNSTONE_APPLICATION=latency',
        '-DSUNSTONE_A733_OS_CPU_RETURN=ON',
        '-DSUNSTONE_A733_OS_CPU_CLOCK=ON',
        '-DA733_A76_ADMITTED=ON',
    ], 'target': 'sunstone-a733-os-latency', 'elf': 'sunstone-a733-os-latency.elf'},
    # the supervised SD0 read image (apps/storage-read); the host session is modeled by checks/storage-read-fixture.py (tools/gate_session.py), at profile 2, or the profile in the image name (storage-read-p1: profile 1)
    'storage-read': {'configure': [
        '-DSUNSTONE_A733_STORAGE_READ=ON',
    ], 'target': 'sunstone-a733-storage-read', 'elf': 'sunstone-a733-storage-read.elf'},
    # the supervised SD0 read image (apps/storage-read); the host session is modeled by checks/storage-read-fixture.py (tools/gate_session.py), at profile 2, or the profile in the image name (storage-read-p1: profile 1)
    'storage-read-p1': {'configure': [
        '-DSUNSTONE_A733_STORAGE_READ=ON',
    ], 'target': 'sunstone-a733-storage-read', 'elf': 'sunstone-a733-storage-read.elf'},
    # the cold-entry monitor with the stage-2 slot loader (boot/a733/stage2); the stage-2 and standalone-boot checks load its ELF and binary from the build directory
    'stage2': {'configure': [
        '-DSUNSTONE_A733_STAGE2=ON',
    ], 'target': 'a733-stage2', 'elf': 'a733-stage2.elf'},
    # the returning scanning radio image
    'ret-radio-scan': {'configure': [
        '-DSUNSTONE_APPLICATION=radio-scan',
        '-DSUNSTONE_A733_OS_RADIO=ON',
        '-DSUNSTONE_A733_OS_RADIO_SCAN=ON',
        '-DSUNSTONE_A733_OS_CPU_RETURN=ON',
        '-DSUNSTONE_A733_OS_CPU_CLOCK=ON',
        '-DA733_A76_ADMITTED=ON',
    ], 'target': 'sunstone-a733-os-radio-scan', 'elf': 'sunstone-a733-os-radio-scan.elf'},
}
BUNDLES = {
    'opp33': ['--bundle', '33'],
    'opp33-dcdc3': ['--bundle', '33', '--cpu-return-dcdc3-800'],
    'storage-read-returning': {'builder': 'apps/storage-read-returning/build.py', 'args': []},
}
