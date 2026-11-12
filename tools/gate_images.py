"""The images and bundles tools/integration-gate.py builds for the checks that name them in GATE_RUNS (checks/check-index.py).

IMAGES are CMake recipes built once per tree state, with one target or a list of targets built in one invocation; the gate's
OS variants (VARIANTS) are named directly and need no recipe.
{elf:X} and {build:X} are an image's ELF and build directory, {session:X} the session bytes tools/gate_session.py builds for
image X, {bundle:X} the probe ELF that apps/board-bundles-power/build-opp.py builds for bundle X (BUNDLES lists its arguments),
{fixture:X} the directory of fixture X (FIXTURES). A fixture is host data that checks read and that a cleanup of the cache may delete,
so the gate runs its idempotent build command before the checks that name it and a build that is current does nothing.
"""

FIXTURES = {
    # the compiled test graphs and their input and simulator reference tensors (tools/npu-graph-build.py, tools/npu-fixtures.json)
    'npu-graphs': {'directory': 'build/npu-graph-variants', 'build': ['tools/npu-graph-build.py', '--output', '{directory}']},
}

IMAGES = {
    'os-usb-ledger': {'configure': [
        '-DSUNSTONE_A733_OS=ON', '-DSUNSTONE_APPLICATION=hello-compute',
        '-DSUNSTONE_A733_OS_USB_CLASSES=ON', '-DSUNSTONE_BOARD_USB2_IRQ=ON',
    ], 'target': 'sunstone-a733-os-hello', 'elf': 'sunstone-a733-os-hello.elf'},
    'ret-walks-uncached': {'configure': [
        '-DSUNSTONE_APPLICATION=table-walks', '-DSUNSTONE_A733_TABLE_WALKS=uncached',
    ], 'target': 'sunstone-a733-os-table-walks', 'elf': 'sunstone-a733-os-table-walks.elf'},
    'ret-walks-cached': {'configure': [
        '-DSUNSTONE_APPLICATION=table-walks', '-DSUNSTONE_A733_TABLE_WALKS=cached',
    ], 'target': 'sunstone-a733-os-table-walks', 'elf': 'sunstone-a733-os-table-walks.elf'},
    'resident-stop': {'configure': [
        '-DSUNSTONE_A733_OS=ON', '-DSUNSTONE_APPLICATION=hello-compute',
        '-DSUNSTONE_A733_OS_RESIDENT_STOP_ON_FINISH=ON',
    ], 'target': 'sunstone-a733-os-hello', 'elf': 'sunstone-a733-os-hello.elf'},
    # the returning NPU image of apps/cpu-npu-concurrency (apps/cpu-npu-concurrency/README.md)
    'ret-npu-load': {'configure': [
        '-DSUNSTONE_MIMUS_BOARD_APPLICATION=ON',
        '-DSUNSTONE_APPLICATION=cpu-npu-load',
        '-DSUNSTONE_APPLICATION_cpu-npu-load_ROOT={source}/apps/cpu-npu-concurrency',
        '-DSUNSTONE_CPU_NPU_ARTIFACTS={cache}/build/npu-board/session-artifacts.json',
    ], 'target': 'sunstone-a733-os', 'elf': 'sunstone-a733-os.elf'},
    # the returning GPU image of apps/gpu-lifecycle (apps/gpu-lifecycle/README.md)
    'ret-gpu': {'configure': [
        '-DSUNSTONE_APPLICATION=gpu-lifecycle',
        '-DSUNSTONE_A733_OS_GPU=ON',
    ], 'target': 'sunstone-a733-os-gpu-lifecycle', 'elf': 'sunstone-a733-os-gpu-lifecycle.elf'},
    # the returning latency diagnostic image
    'ret-latency': {'configure': [
        '-DSUNSTONE_LATENCY=ON',
        '-DSUNSTONE_APPLICATION=latency',
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
    # the emulator models of the FreeRTOS SMP port (cmake/smp.cmake) that checks/smp-ticket-lock.py runs under QEMU
    'smp-models': {'configure': [
        '-DSUNSTONE_PARTITIONED_SCHEDULER=ON',
    ], 'target': ['smp-kernel-model', 'smp-context-model', 'smp8-soak-model'], 'elf': 'smp-kernel-model.elf'},
    # the returning scanning radio image
    'ret-radio-scan': {'configure': [
        '-DSUNSTONE_APPLICATION=radio-scan',
        '-DSUNSTONE_A733_OS_RADIO_SCAN=ON',
    ], 'target': 'sunstone-a733-os-radio-scan', 'elf': 'sunstone-a733-os-radio-scan.elf'},
}
BUNDLES = {
    'opp33': ['--bundle', '33'],
    'opp33-dcdc3': ['--bundle', '33', '--cpu-return-dcdc3-800'],
    'storage-read-returning': {'builder': 'apps/storage-read-returning/build.py', 'args': []},
}
