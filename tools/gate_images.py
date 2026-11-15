"""The images and bundles tools/integration-gate.py builds for the checks that name them in GATE_RUNS (checks/check-index.py).

IMAGES are CMake recipes built once per tree state, with one target or a list of targets built in one invocation (a recipe with
'project' configures that directory of the checkout on its own: tools/build_graph.configure_command); the gate's
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
    # the returning video decoder check image (apps/vdec-check, docs/video-decode-os.md), whose fault return
    # checks/os-vdec-fault-return.py runs
    'ret-vdec': {'configure': [
        '-DSUNSTONE_APPLICATION=vdec-check',
    ], 'target': 'sunstone-a733-os-vdec-check', 'elf': 'sunstone-a733-os-vdec-check.elf'},
    'os-usb-ledger': {'configure': [
        '-DSUNSTONE_IRQ_FP_UNSAVED=ON',
        '-DSUNSTONE_A733_OS=ON', '-DSUNSTONE_APPLICATION=hello-compute',
        '-DSUNSTONE_A733_OS_USB_CLASSES=ON', '-DSUNSTONE_BOARD_USB2_IRQ=ON',
    ], 'target': 'sunstone-a733-os-hello', 'elf': 'sunstone-a733-os-hello.elf'},
    'card-fel-recovery': {'configure': [
        '-DSUNSTONE_A733_CARD_FEL_RECOVERY=ON',
    ], 'target': 'a733-card-fel-recovery', 'elf': 'a733-card-fel-recovery.elf'},
    'ret-walks-uncached': {'configure': [
        '-DSUNSTONE_IRQ_FP_UNSAVED=ON',
        '-DSUNSTONE_APPLICATION=table-walks', '-DSUNSTONE_A733_TABLE_WALKS=uncached',
    ], 'target': 'sunstone-a733-os-table-walks', 'elf': 'sunstone-a733-os-table-walks.elf'},
    'ret-walks-cached': {'configure': [
        '-DSUNSTONE_IRQ_FP_UNSAVED=ON',
        '-DSUNSTONE_APPLICATION=table-walks', '-DSUNSTONE_A733_TABLE_WALKS=cached',
    ], 'target': 'sunstone-a733-os-table-walks', 'elf': 'sunstone-a733-os-table-walks.elf'},
    'resident-stop': {'configure': [
        '-DSUNSTONE_IRQ_FP_UNSAVED=ON',
        '-DSUNSTONE_A733_OS=ON', '-DSUNSTONE_APPLICATION=hello-compute',
        '-DSUNSTONE_A733_OS_RESIDENT_STOP_ON_FINISH=ON',
    ], 'target': 'sunstone-a733-os-hello', 'elf': 'sunstone-a733-os-hello.elf'},
    'card-profile': {'configure': [
        '-DSUNSTONE_IRQ_FP_UNSAVED=ON',
        '-DSUNSTONE_A733_OS=ON', '-DSUNSTONE_APPLICATION=hello-compute',
        '-DSUNSTONE_A733_OS_RESIDENT_STOP_ON_FINISH=OFF',
    ], 'target': 'sunstone-a733-os-hello', 'elf': 'sunstone-a733-os-hello.elf'},
    # the returning NPU image of apps/cpu-npu-concurrency (apps/cpu-npu-concurrency/README.md)
    'ret-npu-load': {'configure': [
        '-DSUNSTONE_IRQ_FP_UNSAVED=ON',
        '-DSUNSTONE_MIMUS_BOARD_APPLICATION=ON',
        '-DSUNSTONE_APPLICATION=cpu-npu-load',
        '-DSUNSTONE_APPLICATION_cpu-npu-load_ROOT={source}/apps/cpu-npu-concurrency',
        '-DSUNSTONE_CPU_NPU_ARTIFACTS={cache}/build/npu-board/session-artifacts.json',
    ], 'target': 'sunstone-a733-os', 'elf': 'sunstone-a733-os.elf'},
    # the returning GPU image of apps/gpu-lifecycle (apps/gpu-lifecycle/README.md)
    'ret-gpu': {'configure': [
        '-DSUNSTONE_IRQ_FP_UNSAVED=ON',
        '-DSUNSTONE_APPLICATION=gpu-lifecycle',
        '-DSUNSTONE_A733_OS_GPU=ON',
    ], 'target': 'sunstone-a733-os-gpu-lifecycle', 'elf': 'sunstone-a733-os-gpu-lifecycle.elf'},
    # the returning G2D check image (apps/g2d-check, docs/g2d-os.md), whose fault return checks/os-g2d-fault-return.py
    # and operation path checks/os-g2d-operation.py run
    'ret-g2d': {'configure': [
        '-DSUNSTONE_IRQ_FP_UNSAVED=ON',
        '-DSUNSTONE_APPLICATION=g2d-check',
    ], 'target': 'sunstone-a733-os-g2d-check', 'elf': 'sunstone-a733-os-g2d-check.elf'},
    # the returning RTC alarm image (apps/rtc-alarm), whose fault return checks/os-rtc-fault-return.py runs
    'ret-rtc': {'configure': [
        '-DSUNSTONE_APPLICATION=rtc-alarm',
    ], 'target': 'sunstone-a733-os-rtc-alarm', 'elf': 'sunstone-a733-os-rtc-alarm.elf'},
    # the returning latency diagnostic image
    'ret-latency': {'configure': [
        '-DSUNSTONE_IRQ_FP_UNSAVED=ON',
        '-DSUNSTONE_LATENCY=ON',
        '-DSUNSTONE_APPLICATION=latency',
    ], 'target': 'sunstone-a733-os-latency', 'elf': 'sunstone-a733-os-latency.elf'},
    # the latency image of the cores 1-7 bar on the board: CPU0's tick releases CPU0 and CPU5 (mask 0x21), CPUs 4-7 stream,
    # with the cause, wake-segment, restore-span and scheduler traces and the L3 partition of the stage 5 baseline; the other
    # cores' wake tasks wait one tick on their own deadlines
    'ret-latency-bar': {'configure': [
        '-DSUNSTONE_IRQ_FP_UNSAVED=ON',
        '-DSUNSTONE_LATENCY=ON',
        '-DSUNSTONE_APPLICATION=latency',
        '-DSUNSTONE_LATENCY_CAUSE=ON',
        '-DSUNSTONE_LATENCY_L3_PARTITION=ON',
        '-DSUNSTONE_LATENCY_RELEASE_MASK=0x21',
        '-DSUNSTONE_LATENCY_RESTORE_SPANS=ON',
        '-DSUNSTONE_LATENCY_STREAM_MASK=0xf0',
        '-DSUNSTONE_LATENCY_WAKE_SEGMENTS=ON',
        '-DSUNSTONE_SCHED_LATENCY_TRACE=ON',
    ], 'target': 'sunstone-a733-os-latency', 'elf': 'sunstone-a733-os-latency.elf'},
    # the same image whose wake tasks outside the release mask wait with the precise deadline wait (runtime/deadline.c) for the
    # same phase (apps/latency/workload.c, SUNSTONE_LATENCY_PRECISE)
    'ret-latency-bar-precise': {'configure': [
        '-DSUNSTONE_IRQ_FP_UNSAVED=ON',
        '-DSUNSTONE_LATENCY=ON',
        '-DSUNSTONE_APPLICATION=latency',
        '-DSUNSTONE_LATENCY_CAUSE=ON',
        '-DSUNSTONE_LATENCY_L3_PARTITION=ON',
        '-DSUNSTONE_LATENCY_RELEASE_MASK=0x21',
        '-DSUNSTONE_LATENCY_RESTORE_SPANS=ON',
        '-DSUNSTONE_LATENCY_STREAM_MASK=0xf0',
        '-DSUNSTONE_LATENCY_WAKE_SEGMENTS=ON',
        '-DSUNSTONE_SCHED_LATENCY_TRACE=ON',
        '-DSUNSTONE_LATENCY_PRECISE=ON',
    ], 'target': 'sunstone-a733-os-latency', 'elf': 'sunstone-a733-os-latency.elf'},
    # the supervised SD0 read image (apps/storage-read); the host session is modeled by checks/storage-read-fixture.py (tools/gate_session.py), at profile 2, or the profile in the image name (storage-read-p1: profile 1)
    'storage-read': {'configure': [
        '-DSUNSTONE_IRQ_FP_UNSAVED=ON',
        '-DSUNSTONE_A733_STORAGE_READ=ON',
    ], 'target': 'sunstone-a733-storage-read', 'elf': 'sunstone-a733-storage-read.elf'},
    # the supervised SD0 read image (apps/storage-read); the host session is modeled by checks/storage-read-fixture.py (tools/gate_session.py), at profile 2, or the profile in the image name (storage-read-p1: profile 1)
    'storage-read-p1': {'configure': [
        '-DSUNSTONE_IRQ_FP_UNSAVED=ON',
        '-DSUNSTONE_A733_STORAGE_READ=ON',
    ], 'target': 'sunstone-a733-storage-read', 'elf': 'sunstone-a733-storage-read.elf'},
    # the cold-entry monitor with the stage-2 slot loader (boot/a733/stage2); the stage-2 and standalone-boot checks load its ELF and binary from the build directory
    'stage2': {'configure': [
        '-DSUNSTONE_A733_STAGE2=ON',
    ], 'target': 'a733-stage2', 'elf': 'a733-stage2.elf'},
    # the diagnostic stage 2 that parks after its WDT0 0x41 arming (docs/designs/card-boot-chain.md, "Measuring the OS row")
    'stage2-armed-stop': {'configure': [
        '-DSUNSTONE_A733_STAGE2=ON',
    ], 'target': 'a733-stage2-armed-stop', 'elf': 'a733-stage2-armed-stop.elf'},
    # the emulator models of the FreeRTOS SMP port (cmake/smp.cmake) that checks/smp-ticket-lock.py runs under QEMU
    'smp-models': {'configure': [
        '-DSUNSTONE_PARTITIONED_SCHEDULER=ON',
    ], 'target': ['smp-kernel-model', 'smp-context-model', 'smp8-soak-model'], 'elf': 'smp-kernel-model.elf'},
    'scheduler-control-models': {'configure': [
        '-DSUNSTONE_PARTITIONED_SCHEDULER=ON',
    ], 'target': ['scheduler-partitioned-qemu', 'scheduler-partitioned-storm-qemu',
                  'scheduler-objects-qemu', 'scheduler-yield-qemu', 'scheduler-yield-storm-qemu',
                  'scheduler-partitioned-heap-qemu'], 'elf': 'scheduler-partitioned-qemu.elf'},
    'latency-control-busy-model': {'configure': [
        '-DSUNSTONE_PARTITIONED_SCHEDULER=ON', '-DSUNSTONE_LATENCY=ON',
        '-DSUNSTONE_SCHED_LATENCY_TRACE=ON', '-DSUNSTONE_LATENCY_RESTORE_SPANS=ON',
        '-DSUNSTONE_LATENCY_CONTROL=ON', '-DSUNSTONE_LATENCY_CONTROL_MODE=busy',
    ], 'target': 'latency-control-model', 'elf': 'latency-control-model.elf'},
    'latency-control-idle-model': {'configure': [
        '-DSUNSTONE_PARTITIONED_SCHEDULER=ON', '-DSUNSTONE_LATENCY=ON',
        '-DSUNSTONE_SCHED_LATENCY_TRACE=ON', '-DSUNSTONE_LATENCY_RESTORE_SPANS=ON',
        '-DSUNSTONE_LATENCY_CONTROL=ON', '-DSUNSTONE_LATENCY_CONTROL_MODE=idle',
    ], 'target': 'latency-control-model', 'elf': 'latency-control-model.elf'},
    # the eight-core latency cause model (cmake/latency.cmake) that checks/tick-footprint-budget.py traces under QEMU
    'latency-cause-model': {'configure': [
        '-DSUNSTONE_PARTITIONED_SCHEDULER=ON', '-DSUNSTONE_LATENCY=ON',
        '-DSUNSTONE_SCHED_LATENCY_TRACE=ON', '-DSUNSTONE_LATENCY_CAUSE=ON',
    ], 'target': 'latency-partitioned-pmu-model', 'elf': 'latency-partitioned-pmu-model.elf'},
    # the same model with the stage-3 board image's measurement sections (band switch, wake segments) and its stream mask; without
    # the band probe's end at CPU3's last sample (apps/latency/workload.c, band_window) its CPUs 1 and 2 stalled in the busy phase
    'latency-wake-model': {'configure': [
        '-DSUNSTONE_PARTITIONED_SCHEDULER=ON', '-DSUNSTONE_LATENCY=ON',
        '-DSUNSTONE_SCHED_LATENCY_TRACE=ON', '-DSUNSTONE_LATENCY_CAUSE=ON',
        '-DSUNSTONE_LATENCY_BAND_SWITCH=ON', '-DSUNSTONE_LATENCY_WAKE_SEGMENTS=ON', '-DSUNSTONE_LATENCY_STREAM_MASK=0xf0',
    ], 'target': 'latency-partitioned-pmu-model', 'elf': 'latency-partitioned-pmu-model.elf'},
    # the same model with the stage-5 board image's release mask 0x21: CPUs 1-4, 6 and 7 wait on their own deadlines and CPU0's
    # tick releases CPUs 0 and 5; without each core's own timer arm (kernel/scheduler/freertos.c, ssKernelLocalArm) those cores
    # never wake, since CPU0's tick no longer reads their deadlines, and the model stops at its run budget
    'latency-local-model': {'configure': [
        '-DSUNSTONE_PARTITIONED_SCHEDULER=ON', '-DSUNSTONE_LATENCY=ON',
        '-DSUNSTONE_SCHED_LATENCY_TRACE=ON', '-DSUNSTONE_LATENCY_CAUSE=ON',
        '-DSUNSTONE_LATENCY_WAKE_SEGMENTS=ON', '-DSUNSTONE_LATENCY_STREAM_MASK=0xf0',
        '-DSUNSTONE_LATENCY_RELEASE_MASK=0x21',
    ], 'target': 'latency-partitioned-pmu-model', 'elf': 'latency-partitioned-pmu-model.elf'},
    # the same model whose wake tasks on CPUs 1-4, 6 and 7 wait with the precise deadline wait for the same phase
    # (apps/latency/workload.c, SUNSTONE_LATENCY_PRECISE), as the board's ret-latency-bar-precise image does
    'latency-precise-model': {'configure': [
        '-DSUNSTONE_PARTITIONED_SCHEDULER=ON', '-DSUNSTONE_LATENCY=ON',
        '-DSUNSTONE_SCHED_LATENCY_TRACE=ON', '-DSUNSTONE_LATENCY_CAUSE=ON',
        '-DSUNSTONE_LATENCY_WAKE_SEGMENTS=ON', '-DSUNSTONE_LATENCY_STREAM_MASK=0xf0',
        '-DSUNSTONE_LATENCY_RELEASE_MASK=0x21', '-DSUNSTONE_LATENCY_PRECISE=ON',
    ], 'target': 'latency-partitioned-pmu-model', 'elf': 'latency-partitioned-pmu-model.elf'},
    # the other host model images that link the latency workloads (cmake/latency.cmake); one configure builds them all, so the
    # images that checks/latency-control-integration.py runs and these are built by the same recipe options
    'latency-models': {'configure': [
        '-DSUNSTONE_PARTITIONED_SCHEDULER=ON', '-DSUNSTONE_LATENCY=ON',
        '-DSUNSTONE_SCHED_LATENCY_TRACE=ON', '-DSUNSTONE_LATENCY_RESTORE_SPANS=ON',
        '-DSUNSTONE_LATENCY_CONTROL=ON', '-DSUNSTONE_LATENCY_CONTROL_MODE=busy',
    ], 'target': ['latency-model', 'latency-partitioned-writes-model', 'latency-partitioned-loads-model',
                  'latency-partitioned-pmu-model'], 'elf': 'latency-model.elf'},
    # standalone model projects beside the top-level one: 'project' is the CMake source directory, configured with the toolchain file
    # and the pinned kernel only (no cfg.sh, which configures the top-level project)
    'latency-cost-model': {'project': 'apps/latency-cost', 'configure': ['-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'],
                           'target': 'latency-cost-model', 'elf': 'latency-cost-model.elf'},
    'cpu-reliability-model': {'project': 'checks/cpu-reliability-model',
                              'configure': ['-DSUNSTONE_IRQ_FP_UNSAVED=ON', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'],
                              'target': 'a733-smp-cached-diagnostic', 'elf': 'a733-smp-cached-diagnostic.elf'},
    # the A733 board and diagnostic images outside the OS, with the options that add the board applications;
    # checks/irq-general-regs.py walks every executable it links with configSS_IRQ_SAVES_FP 0. The OS option is off,
    # after the gate's own -DSUNSTONE_A733_OS=ON: with it, the storage-maintenance image does not link.
    'a733-applications': {'configure': [
        '-DSUNSTONE_IRQ_FP_UNSAVED=ON',
        '-DSUNSTONE_A733_OS=OFF',
        '-DSUNSTONE_A733_ADC=ON', '-DSUNSTONE_A733_DEINTERLACE=ON', '-DSUNSTONE_A733_MONITOR=ON',
        '-DSUNSTONE_A733_PCIE=ON', '-DSUNSTONE_A733_PCIE_NVME=ON', '-DSUNSTONE_A733_PCIE_TEST=ON',
        '-DSUNSTONE_A733_PWM=ON', '-DSUNSTONE_A733_RTC=ON', '-DSUNSTONE_A733_SPI=ON', '-DSUNSTONE_A733_THERMAL=ON',
        '-DSUNSTONE_BLUETOOTH_APPLICATION=ON', '-DSUNSTONE_JPEG_ENCODE_APPLICATION=ON',
    ], 'target': [
        'a733-smp-cached-diagnostic', 'a733-smp-observe', 'a733-smp-supervised', 'sunstone-a733-smp-cached-devices',
        'sunstone-a733-storage-maintenance', 'sunstone-a733-board', 'sunstone-a733-camera', 'sunstone-a733-gpu-render',
        'sunstone-a733-hdmi', 'sunstone-a733-adc', 'sunstone-a733-bluetooth', 'sunstone-a733-deinterlace',
        'sunstone-a733-jpeg-encode', 'sunstone-a733-monitor', 'sunstone-a733-pcie', 'sunstone-a733-pcie-nvme',
        'sunstone-a733-pcie-test', 'sunstone-a733-pwm', 'sunstone-a733-rtc', 'sunstone-a733-spi', 'sunstone-a733-thermal',
    ], 'elf': 'a733-smp-cached-diagnostic.elf'},
    # the returning scanning radio image
    'ret-radio-scan': {'configure': [
        '-DSUNSTONE_IRQ_FP_UNSAVED=ON',
        '-DSUNSTONE_APPLICATION=radio-scan',
        '-DSUNSTONE_A733_OS_RADIO_SCAN=ON',
    ], 'target': 'sunstone-a733-os-radio-scan', 'elf': 'sunstone-a733-os-radio-scan.elf'},
}
BUNDLES = {
    'opp33': ['--bundle', '33'],
    'opp33-dcdc3': ['--bundle', '33', '--cpu-return-dcdc3-800'],
    'storage-read-returning': {'builder': 'apps/storage-read-returning/build.py', 'args': []},
}


def project_flags(cache):
    """The cache variables of a standalone model project: the toolchain file, the pinned kernel and the linker."""
    sources = cache / 'sources'
    return ['-DCMAKE_TOOLCHAIN_FILE=%s' % '{source}/cmake/aarch64-clang.cmake',
            '-DFREERTOS_DIR=%s' % (sources / 'FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'),
            '-DSUNSTONE_LLD=%s' % (cache / 'host/usr/lib/llvm-21/bin/ld.lld')]
