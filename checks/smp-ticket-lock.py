#!/usr/bin/env python3
"""Exhaust ticket admission, recursion and monitored waits on three cores.

The state model assumes coherent normal memory and Arm acquire/release ordering.
It checks every interleaving of the modeled operations, including owner wrap.
Every image is built for the Large System Extensions, so a ticket is taken with
one LDADDA. The model does not cover weak-memory propagation or qualify
hardware. QEMU then runs the compiled FreeRTOS port on the two board
cores, Cortex-A55 and Cortex-A76, and the instruction trace must show the
LDADDA take, the STLRH release and, under the contention that the kernel model
forces, the SEVL, WFE and LDAXRH wait, and no exclusive-load take.
"""
import argparse
from collections import deque
import hashlib
import json
from pathlib import Path
import re
import subprocess

from harness import CACHE, environment

CHECK_INPUTS = ['patches/freertos-smp-c.patch', 'patches/freertos-smp-macro.patch',
                'checks/smp-kernel.c', 'checks/smp-context.c', 'checks/smp8-soak.c']
GATE_RUNS = [['--build', '{build:smp-models}', '--output', '{output}']]
CPUS = ("cortex-a55", "cortex-a76")

MASK = 0xFFFF


def explore(seed, rounds):
    # Core state: phase, completed rounds, ticket.
    initial = (seed | seed << 16, (("start", 0, 0),) * 3, ())
    seen, work = {initial}, deque([initial])
    terminal = edges = waits = recursions = 0
    while work:
        word, cores, queue = work.popleft()
        owner, next_ticket = word & MASK, word >> 16
        held = [i for i, c in enumerate(cores) if c[0] in ("held", "recursive", "outer")]
        assert len(held) <= 1, ("mutual exclusion", word, cores)
        assert (next_ticket - owner) & MASK == len(queue)
        assert len(queue) <= 3
        if held:
            assert queue[0] == held[0], ("FIFO", queue, held)
        successors = []
        for core, (phase, done, ticket) in enumerate(cores):
            def step(new_phase, *, new_word=word, new_queue=queue, new_done=done, new_ticket=ticket):
                changed = list(cores)
                changed[core] = (new_phase, new_done, new_ticket)
                successors.append((new_word, tuple(changed), new_queue))
            if phase == "start":
                # LDADDA adds one to the next-ticket halfword and returns the old word in one access.
                step("wait", new_word=(word + (1 << 16)) & 0xFFFFFFFF,
                     new_queue=queue + (core,), new_ticket=next_ticket)
            elif phase == "wait":
                waits += 1
                if owner == ticket:
                    assert queue[0] == core
                    step("held")
            elif phase == "held":
                # Recursive entry increments bookkeeping without taking a ticket.
                recursions += 1
                step("recursive")
            elif phase == "recursive":
                step("outer")
            elif phase == "outer":
                assert queue[0] == core
                step("done" if done + 1 == rounds else "start",
                     new_word=(word & 0xFFFF0000) | ((owner + 1) & MASK),
                     new_queue=queue[1:], new_done=done + 1)
            else:
                assert phase == "done"
        if not successors:
            assert all(c[0] == "done" for c in cores), ("deadlock", word, cores, queue)
            assert not queue and owner == next_ticket
            terminal += 1
        for state in successors:
            edges += 1
            if state not in seen:
                seen.add(state)
                work.append(state)
        assert len(seen) < 750000, "model exceeded its bounded state budget"
    assert terminal and recursions and waits
    return dict(seed=seed, rounds=rounds, states=len(seen), edges=edges, terminal_states=terminal)


def monitored_wait():
    # Model SEVL; WFE; LDAXRH; CMP; B.NE and one concurrent STLRH.
    # State: instruction, event bit, reservation, observed owner, actual owner.
    initial = ("sevl", False, False, -1, 0)
    seen, work, finished = {initial}, [initial], 0
    while work:
        phase, event, monitor, observed, owner = work.pop()
        following = []
        if owner == 0:
            # A store clears a global reservation and sets that PE's event bit.
            following.append((phase, event or monitor, False, observed, 1))
        if phase == "sevl":
            following.append(("wfe", True, monitor, observed, owner))
        elif phase == "wfe" and event:
            following.append(("load", False, monitor, observed, owner))
        elif phase == "load":
            following.append(("compare", event, True, owner, owner))
        elif phase == "compare":
            following.append(("done" if observed == 1 else "wfe", event, monitor, observed, owner))
        if phase == "done":
            assert owner == 1
            finished += 1
        elif owner == 1:
            assert following, ("lost owner-store wakeup", phase, event, monitor, observed)
        for state in following:
            if state not in seen:
                seen.add(state)
                work.append(state)
    assert finished
    return dict(states=len(seen), completed_states=finished)


def arithmetic():
    # Every 16-bit starting owner, with every possible three-core queue depth.
    for owner in range(65536):
        for depth in range(4):
            next_ticket = (owner + depth) & MASK
            word = owner | next_ticket << 16
            updated = (word + (1 << 16)) & 0xFFFFFFFF
            assert updated & MASK == owner
            assert updated >> 16 == (next_ticket + 1) & MASK
            released = (word & 0xFFFF0000) | ((owner + 1) & MASK)
            assert released >> 16 == next_ticket
    return 65536 * 4


def tas_negative_control():
    # Search bounded TAS try/release transitions on three cores. Core 0 has
    # requested the lock while core 1 holds it; every nonholder may try.
    # Require victim progress between overtakes, so the witness does not
    # depend on preventing core 0 from executing its acquisition loop.
    cores, victim, bound = 3, 0, 2
    # State: holder (-1 when free), overtakes, victim tried since handoff,
    # and cores that executed a transition. No state has more than bound+1
    # overtakes; a successful victim acquisition ends that search branch.
    initial = (1, 0, False, 0)
    parents = {initial: None}
    work = deque([initial])
    transitions = 0
    while work:
        state = work.popleft()
        holder, overtakes, probed, ran = state
        if overtakes > bound and ran == (1 << cores) - 1:
            # This is the actual N-1 invariant, expected to reject TAS.
            try:
                assert overtakes <= bound, "TAS overtaking exceeds N-1"
            except AssertionError as error:
                trace = []
                cursor = state
                while parents[cursor] is not None:
                    previous, core, operation = parents[cursor]
                    trace.append(dict(core=core, operation=operation,
                                      before=list(previous), after=list(cursor)))
                    cursor = previous
                trace.reverse()
                return dict(fifo=False, cores=cores, victim=victim,
                            overtaking_bound=bound, observed_overtakes=overtakes,
                            expected_invariant_failure=str(error),
                            states=len(parents), transitions=transitions, trace=trace)
        if overtakes > bound:
            continue
        for core in range(cores):
            visited = ran | (1 << core)
            if holder == core:
                following, operation = (-1, overtakes, probed, visited), "release"
            elif holder >= 0:
                following = (holder, overtakes, probed or core == victim, visited)
                operation = "try_busy"
            elif core == victim:
                # The victim acquired; this branch cannot witness starvation.
                continue
            else:
                # A free TAS gate admits any caller. Keep witnesses with a
                # failed victim try before each overtaking acquisition.
                if not probed:
                    continue
                following = (core, overtakes + 1, False, visited)
                operation = "try_acquire"
            transitions += 1
            if following not in parents:
                parents[following] = (state, core, operation)
                work.append(following)
    raise AssertionError("TAS negative control found no overtaking violation")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def qemu_checks(build, emulator, output, cpus):
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    reports = []
    for cpu in cpus:
        # The Cortex-A55 runs the two-core kernel model only; the Cortex-A76 runs all three.
        for name, cores, marker in (("smp-kernel-model", 2, "PASS SMP: 256"),
                                    ("smp-context-model", 2, "PASS SMP context: 64"),
                                    ("smp8-soak-model", 8, "PASS SMP8 soak: 64-lap")):
            if cpu == "cortex-a55" and name != "smp-kernel-model":
                continue
            image = build / (name + ".elf")
            trace = output / (cpu + "-" + name + ".asm.log")
            command = [str(emulator), "-machine", "virt,gic-version=3,secure=off,virtualization=off",
                       "-accel", "tcg,thread=multi", "-cpu", cpu, "-smp", str(cores), "-m", "128M",
                       "-nographic", "-monitor", "none", "-semihosting-config", "enable=on,target=native",
                       "-device", f"loader,file={image.resolve()},cpu-num=0", "-d", "in_asm", "-D", str(trace)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=300, env=environment())  # a hang bound only; the guest asserts no time
            log = result.stdout + result.stderr
            (output / (cpu + "-" + name + ".run.log")).write_text(log)
            assert result.returncode == 0 and marker in log and "FAIL" not in log, (cpu, name, log)
            # This QEMU build prints OBJD-T bytes instead of instruction names. Each translated block is logged once.
            instructions = set()
            for match in re.finditer(r"OBJD-T: ([0-9a-f]+)", trace.read_text()):
                for instruction in decoder.disasm(bytes.fromhex(match[1]), 0):
                    instructions.add(instruction.mnemonic)
            # vPortRecursiveLock takes a ticket with LDADDA and releases with STLRH; no exclusive-load take exists.
            # Under the partitioned scheduler the tick takes no global lock, so only the kernel model, which takes
            # both locks itself, is sure to run a take; the other images must still run no exclusive-load loop.
            locked = name == "smp-kernel-model"
            if locked:
                assert "ldadda" in instructions and "stlrh" in instructions, (cpu, name, sorted(instructions))
            assert not instructions & {"ldaxr", "ldxr", "stxr", "stlxr"}, (cpu, name, sorted(instructions & {"ldaxr", "ldxr", "stxr", "stlxr"}))
            waited = {"sevl", "wfe", "ldaxrh"} <= instructions
            if name == "smp-kernel-model":
                # The kernel model holds both locks until the other core has taken a ticket behind it.
                assert waited, (cpu, "the forced contention ran no monitored wait")
            reports.append(dict(cpu=cpu, image=name, image_sha256=digest(image), command=command,
                                lse_take_executed=locked, monitored_wait_executed=waited, passed=True))
    return reports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True,
                        help="Build directory that holds the FreeRTOS SMP model images (tools/gate_images.py, smp-models)")
    parser.add_argument("--qemu", type=Path, default=CACHE / "host/usr/bin/qemu-system-aarch64")
    parser.add_argument("--cpu", action="append", choices=CPUS,
                        help="Run only these QEMU CPUs; default runs both board cores")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    cases = [explore(seed, rounds) for seed in (0, 65534) for rounds in (1, 2)]
    report = dict(passed=True, physical_access=False, arithmetic_cases=arithmetic(),
                  interleavings=cases, monitored_wait=monitored_wait(),
                  tas_negative_control=tas_negative_control(),
                  limitations="Coherent acquire/release model; no weak-memory propagation or physical timing qualification.",
                  checker_sha256=digest(Path(__file__)),
                  qemu=qemu_checks(args.build, args.qemu, args.output, args.cpu or CPUS))
    (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS ticket locks: {sum(c['states'] for c in cases)} states; "
          f"{report['arithmetic_cases']} wrap cases; monitored waits; TAS fairness control; "
          f"{len(report['qemu'])} FreeRTOS emulator runs")


if __name__ == "__main__":
    main()
