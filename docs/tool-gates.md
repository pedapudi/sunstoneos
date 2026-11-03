# Host execution gates and retained safety

Host-side Python and C under `tools/` and `apps/` refuse to run against a board unless the inputs and the hardware state they consume are valid. A review record, a frozen-input marker, a queue-ready state, a claim string or a separately reviewed image hash is not execution permission. The tools check the bytes and the state that the operation uses.

## Inputs that gate execution

An image manifest linked into the executable defines the consumed application regions and the result capacity. A fixed native model catalogue defines the bytes its driver accepts. Runtime file inventories cover bytes uploaded to the device or parsed as direct safety inputs. Ancillary source copies, logs and unused manifest or template archives do not affect launch. Object and header records are required when a builder reuses compiled objects whose native layouts depend on those headers.

An image hash, a historical host-source snapshot, a queue-ready state or a claim string does not establish current hardware state.

## Output paths

SMP, device, storage, observer, supervised and returning launchers print the actual evidence directory before acquiring the board. Reusing an evidence parent creates a timestamped child; the printed path identifies that run, and earlier results stay in place.

Grants, slot plans, build outputs, generated inputs and launch bundles require the exact requested destination to be unused and refuse an existing path. They never redirect into a child that a later command might overlook.

## Runtime safety

Physical operations require the selected port's `board_lock`, the USB guard, the target and access checks, and ROM and FEL sanity. Transport handlers keep their bounded exchanges and compare uploaded bytes with their read-back.

Native session and build bindings, memory and DMA ownership, cache publication, shared clock and power coordination, supported clock and voltage limits, native thermal bounds and copied-result integrity are required. Returning development runs require the host-armed watchdog and RTC breadcrumbs. The failure-stop and restoration handlers remain; process cleanup cannot establish physical recovery.

A complete DRAM comparison mismatch with verified restoration and a durable journal refuses launch without quarantining the ROM instance, and DDR initialization may follow that refusal. Incomplete reads, transport loss, failed restoration and failed restoration journaling retain quarantine and ownership.

Storage writes are the backup-first boot update with slot fallback, writes confined to the backed-up microSD data partition, and the user-designated scratch-NVMe restriction. No review or queue record widens a write grant or target extent.

SD profile 3 requires `--allow-sd-1v8` when preparing inputs and when launching them. Before execution, the runner durably records `power_removal_required` and retains ownership after success or failure, including a forced ROM return. The card can remain at 1.8 V until whole-board power removal. A verified supervised refusal before native entry clears the requirement; profile 2 keeps its normal return.

## Host gate scripts

| Script | Gate it provides |
| --- | --- |
| [tools/board.py](../tools/board.py), [tools/board_state.py](../tools/board_state.py) | Runs returning images and services the per-board run queue; validates board state and the pinned rail helper. [The board runner](board-runner.md) describes admission. |
| [tools/usb_guard.py](../tools/usb_guard.py) | The shared USB re-enumeration budget and the host xHCI health check; refuses board work after the controller has died. |
| [tools/usb-access.py](../tools/usb-access.py) | Grants and revokes access to the single attached FEL device; refuses to choose among several. |
| [tools/host_admission.py](../tools/host_admission.py) | Runs the EL3 admission of an OS image on the host, using the image's own admission code, before a board run. |
| [tools/session_lifetime.py](../tools/session_lifetime.py) | Derives the session lifetime of a returning image from its manifest and applies the bounds and refusal terms. |
| [tools/board_usb_power.py](../tools/board_usb_power.py) | Observes and recovers the VBUS enable on PL8 (TYPEC24). |
| [tools/storage-update.py](../tools/storage-update.py), [tools/storage-read-returning.py](../tools/storage-read-returning.py) | Preserve card bytes and prepare bounded native updates; prepare and decode bounded SD0 reads without device access. |
| [tools/host_space.py](../tools/host_space.py), [tools/host_priority.py](../tools/host_priority.py) | Keep the gate, the landing and the board runner off `/tmp`, refuse to start where space is short, and set the scheduling priority of host processes that share the machine with the runner. |
| [tools/integration-gate.py](../tools/integration-gate.py) | Runs explicitly selected builds and checks; never launches a board. See [validation](validation.md#select-the-integration-gate-explicitly). |
| [tools/land.py](../tools/land.py) | Merges a worker branch into `bringup`, runs the gate and the validators, fast-forwards and pushes. |
| [tools/git-hooks/pre-push](../tools/git-hooks/pre-push) | Refuses a push whose outgoing commits add secrets, credential files, session paths or assistant credits. |
| [tools/evidence.py](../tools/evidence.py) | Validates evidence records against the schema, the queue and the status files. See [evidence](evidence.md). |
| [checks/check-index.py](../checks/check-index.py), [checks/affected.py](../checks/affected.py), [checks/gate-inputs.json](../checks/gate-inputs.json) | Classify checks as pure-host or input-needing and select the checks a change affects; register the images that image-executing checks take. |
