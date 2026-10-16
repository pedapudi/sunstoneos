# GPU translation-cache invalidation waits for firmware completion

The native invalidation component is **implemented and host-tested**. It
publishes changed workload page tables, submits a firmware MMU-cache command,
and waits for that command's successful return. Physical invalidation, live
table editing, job admission, and integration into the FreeRTOS GPU service
remain outstanding.

This document assumes familiarity with device drivers and virtual memory.
The [interface](../include/pvr_gpu_flush.h) and
[implementation](../drivers/gpu/pvr_gpu_flush.c) operate beside the checked
[GPU control worker](gpu-control.md) and [command transport](gpu-queues.md).
They extend the protocol needed to use the [workload tables](gpu-vm.md).

## Publish each mapping-change batch before submitting its command

Initialization accepts one batch's highest changed translation level and an
absolute deadline. A leaf change invalidates leaf caches and translation
lookaside buffers. A directory change also invalidates directory caches; a
catalogue change additionally invalidates the root catalogue cache. Every
command requests a firmware completion interrupt.

One bounded platform callback publishes all changed entries through the
required CPU cache operations and barriers. The component then submits the
command once space becomes available. A full queue receives bounded control
service; it does not cause another table publication. Once submitted, the
command is never resubmitted by this component.

The pinned driver's `pvr_mmu_flush_exec()` provides the command format and
completion protocol. Its synchronization update value is zero, which is also
the synchronization allocation's initial value. Reading that word cannot
prove completion. The native component therefore collects the reserved
command-return slot and requires a result of one: executed, with no cleanup
busy or firmware polling-failure flags.

The command points to the existing uncached MMU synchronization allocation
defined by [firmware initialization](gpu-fw-state.md). That allocation must
remain mapped for the operation. The component does not create another
synchronization allocation or overwrite firmware-owned memory.

## The owner prevents concurrent mapping use and retains uncertain memory

The privileged owner must stop jobs from using affected mappings before
changing their tables. It must also close admission to new jobs, retain the
table and data allocations, and hold power resources. Tables remain unchanged
between operation initialization and completion. The same worker serializes
invalidation, controller calls, and other command producers.

While waiting, each poll services at most four firmware events and one return
scan. Completion may be reported while unrelated events remain queued; the
normal control worker must continue draining them. The command deadline covers
CPU publication, queue pressure, and firmware response. Time samples must not
precede the latest samples from the operation, controller, or transport.

The operation requires the same active queue and firmware epoch throughout.
A callback failure, deadline violation, firmware error, or changed lifecycle
state aborts further control and transport I/O. The failed operation retains
its command, token, status, and ownership. Later polls make no callbacks.
Independent device isolation is required before discarding failed resources.

A successful invalidation does not complete GPU jobs, flush GPU data caches,
or isolate the device. Releasing backing memory additionally requires evidence
that no job can access it. The platform publication callback, actual table
edits, allocation ownership, and service scheduling still need implementation
and physical qualification.

## Focused checks exercise the integrated protocol

One [sanitizer check](../checks/pvr-gpu-flush.c) links the real controller,
transport, firmware layout, and invalidation component. Its device model
separates CPU table values, published table values, and cached translations.
It verifies that publication precedes command submission and that queue-read
progress alone cannot complete invalidation.

The same check covers all three translation levels, queue pressure and wrap,
bounded event service, late publication, missing completion, clock regression,
epoch changes, firmware restart, and unsuccessful command results. Callback
failure injection includes writes that take effect before reporting failure.
The main check passes 44 retained-failure cases; a focused additional case
rejects a time sample older than the nested controller's sample. Terminal
polls perform no further backend operations.

The [reference comparison](../checks/pvr-gpu-flush-reference.py) compiles the
complete pinned `pvr_mmu_flush_exec()` function with captured transport calls.
All 88 command bytes match for each translation level, and the reference
performs one submission and one command-return wait. Original structure and
flag definitions supply the payload layout. The reference sources are pinned
to revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` and checked against the
cached board-source manifest. See the
[source function](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/gpu/imagination/pvr_mmu.c).

Clang 21 compiled the component for AArch64 with strict alignment and
freestanding options. The object contains 1,304 bytes of code/read-only data
and no writable static data. Initialization uses a 128-byte frame; polling
uses a 64-byte frame. Called functions add their own stack requirements.

Evidence is under `~/.cache/sunstoneos/evidence/gpu-flush-20261016/`.
The `clock-order` directory contains the current source snapshots, sanitizer
result, focused clock-regression result, target object, and exact commands.
The parent directory contains the original-function comparison. Its command
bytes remain identical after the clock fix, so that comparison was reused.
The affected host check was repeated once after the clock fix. Existing GPU
component, emulator, board, mimus, and performance checks were not repeated.
