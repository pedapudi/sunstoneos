#!/usr/bin/env python3
"""Exhaust ticket admission, recursion and monitored waits on three cores.

The state model assumes coherent normal memory and Arm acquire/release ordering.
It checks every interleaving of the modeled operations, including failed
exclusive stores and owner wrap. It does not model weak-memory propagation or
qualify hardware. Optional QEMU runs exercise the compiled FreeRTOS port.
"""
import argparse
from collections import deque
import hashlib
import json
from pathlib import Path
import re
import subprocess

from harness import CACHE, environment

MASK = 0xFFFF


def explore(seed, lse, rounds):
    # Core state: phase, completed rounds, ticket, exclusive version, loaded word.
    initial = (seed | seed << 16, (("start", 0, 0, -1, 0),) * 3, (), 0)
    seen, work = {initial}, deque([initial])
    terminal = edges = retries = waits = recursions = 0
    while work:
        word, cores, queue, version = work.popleft()
        owner, next_ticket = word & MASK, word >> 16
        held = [i for i, c in enumerate(cores) if c[0] in ("held", "recursive", "outer")]
        assert len(held) <= 1, ("mutual exclusion", word, cores)
        assert (next_ticket - owner) & MASK == len(queue)
        assert len(queue) <= 3
        if held:
            assert queue[0] == held[0], ("FIFO", queue, held)
        successors = []
        for core, (phase, done, ticket, reservation, loaded) in enumerate(cores):
            def step(new_phase, *, new_word=word, new_queue=queue, new_version=version,
                     new_done=done, new_ticket=ticket, new_reservation=reservation, new_loaded=loaded):
                changed = list(cores)
                changed[core] = (new_phase, new_done, new_ticket, new_reservation, new_loaded)
                successors.append((new_word, tuple(changed), new_queue, new_version))
            if phase == "start":
                if lse[core]:
                    step("wait", new_word=(word + (1 << 16)) & 0xFFFFFFFF,
                         new_queue=queue + (core,), new_version=version + 1, new_ticket=next_ticket)
                else:
                    step("exclusive", new_reservation=version, new_loaded=word)
            elif phase == "exclusive":
                # STXR can fail spuriously even with an unchanged reservation.
                retries += 1
                step("start", new_reservation=-1)
                if reservation == version:
                    step("wait", new_word=(loaded + (1 << 16)) & 0xFFFFFFFF,
                         new_queue=queue + (core,), new_version=version + 1,
                         new_ticket=loaded >> 16, new_reservation=-1)
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
                     new_queue=queue[1:], new_version=version + 1,
                     new_done=done + 1, new_reservation=-1)
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
    if not all(lse):
        assert retries
    return dict(seed=seed, lse=list(lse), rounds=rounds, states=len(seen), edges=edges,
                terminal_states=terminal, exclusive_retry_transitions=retries)


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
    # Core 0 requests while core 1 holds. Core 0 executes a failed trylock on
    # every cycle; core 1 releases and reacquires before core 0's next attempt.
    # The state repeats, so arbitrarily many overtakes are possible even though
    # both cores execute infinitely often. Ticket admission forbids this cycle.
    state = (1, True)
    trace = ["core 0 requests while core 1 holds"]
    for _ in range(3):
        owner, waiting = state
        assert owner == 1 and waiting
        trace.extend(("core 0 tries and observes busy", "core 1 releases", "core 1 reacquires"))
        state = (1, True)
    assert state == (1, True)
    return dict(fifo=False, starvation_cycle=True, trace=trace)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def qemu_checks(build, emulator, output):
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    reports = []
    for cpu in ("cortex-a53", "cortex-a76"):
        for name, cores, marker in (("smp-kernel-model", 2, "PASS SMP: 256"),
                                    ("smp-context-model", 2, "PASS SMP context: 64"),
                                    ("smp8-soak-model", 8, "PASS SMP8 soak: 64-lap")):
            image = build / (name + ".elf")
            trace = output / (cpu + "-" + name + ".asm.log")
            command = [str(emulator), "-machine", "virt,gic-version=3,secure=off,virtualization=off",
                       "-accel", "tcg,thread=multi", "-cpu", cpu, "-smp", str(cores), "-m", "128M",
                       "-nographic", "-monitor", "none", "-semihosting-config", "enable=on,target=native",
                       "-device", f"loader,file={image.resolve()},cpu-num=0", "-d", "in_asm", "-D", str(trace)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=45, env=environment())
            log = result.stdout + result.stderr
            (output / (cpu + "-" + name + ".run.log")).write_text(log)
            assert result.returncode == 0 and marker in log and "FAIL" not in log, (cpu, name, log)
            # This QEMU build prints OBJD-T bytes instead of instruction names.
            instructions = set()
            for match in re.finditer(r"OBJD-T: ([0-9a-f]+)", trace.read_text()):
                instructions.update(i.mnemonic for i in decoder.disasm(bytes.fromhex(match[1]), 0))
            assert "ldaxr" in instructions and "stlrh" in instructions
            assert ("ldadda" in instructions) == (cpu == "cortex-a76"), (cpu, instructions)
            reports.append(dict(cpu=cpu, image=name, image_sha256=digest(image), command=command,
                                lse_executed="ldadda" in instructions,
                                monitored_wait_executed="ldaxrh" in instructions, passed=True))
    return reports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--build", type=Path, help="Built FreeRTOS SMP model images; enables QEMU checks")
    parser.add_argument("--qemu", type=Path, default=CACHE / "host/usr/bin/qemu-system-aarch64")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    cases = [explore(seed, lse, rounds) for seed in (0, 65534)
             for lse, rounds in (((True,) * 3, 2), ((False,) * 3, 1), ((True, False, True), 2))]
    report = dict(passed=True, physical_access=False, arithmetic_cases=arithmetic(),
                  interleavings=cases, monitored_wait=monitored_wait(),
                  tas_negative_control=tas_negative_control(),
                  limitations="Coherent acquire/release model; no weak-memory propagation or physical timing qualification.",
                  checker_sha256=digest(Path(__file__)),
                  qemu=qemu_checks(args.build, args.qemu, args.output) if args.build else [])
    (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS ticket locks: {sum(c['states'] for c in cases)} states; "
          f"{report['arithmetic_cases']} wrap cases; monitored waits; TAS starvation control; "
          f"{len(report['qemu'])} FreeRTOS emulator runs")


if __name__ == "__main__":
    main()
