# Reference sources

Pin source revisions before using them in a build. Source files document reference
implementations; physical support on this board requires qualification evidence.

| Source | Inspected revision or document | Purpose |
| --- | --- | --- |
| [Board kernel](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f) | `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` | Board wiring descriptions and A733 driver implementations |
| [Board bootloader](https://github.com/orangepi-xunlong/u-boot-orangepi/tree/b791be842935b27268ae3d00e943a9075495f30a) | `b791be842935b27268ae3d00e943a9075495f30a` | Boot package and application handoff |
| [FreeRTOS kernel](https://github.com/FreeRTOS/FreeRTOS-Kernel/tree/97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2) | `97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2` | AArch64 system-register interrupt port and scheduler |
| [FEL tools](https://github.com/linux-sunxi/sunxi-tools/tree/b92f4ed3115a520e3ec406697d268239e3f29488) | `b92f4ed3115a520e3ec406697d268239e3f29488` | Recovery protocol and tested local A733 patches |
| [A733 datasheet](https://dl.radxa.com/cubie/a7a/docs/hw/datasheet/A733_Datasheet_V0.93.pdf) | Version 0.93 | Hardware inventory and documented limits |
| [PowerVR](https://docs.mesa3d.org/drivers/powervr.html) | Documentation inspected 2026-10-15 | Candidate GPU runtime; exact BVNC match pending |
| [OV5640 driver](https://github.com/torvalds/linux/blob/master/drivers/media/i2c/ov5640.c) | Revision pin pending | Sensor programming reference |

The inspected upstream Linux Allwinner device-tree directory at
`2f2f7eeef8b0a1a9b9ca998231419873ccae8d4e` contains no A733 entry.
Track upstream support per component instead of assuming the board is supported.

The mimus engine has a portable C++ backend and deterministic host comparisons.
Its board adapters require changes for thread identity, allocation, storage,
scheduling, and hardware access. Use its source and current project instructions
when implementing that port.

Download identities and archive hashes are in [sources.lock.json](../sources.lock.json).
The board-specific boot0 binary comes from the board build repository at
`a95129bf30ea11886cc35f08ebb3e69695a26656`. Its header specifies a 245,760-byte
image loaded at `0x47000`. Selected power/clock and DDR functions have executed
through the [RAM wrapper](boot.md); the normal storage boot path has not.

The forced-FEL ROM entry was inspected in the [A733 ROM dump](https://github.com/smaeul/sunxi-blobs/blob/42aabf4f2c93158a0cd88a000f9af56ce024882a/sun60iw2p1/fixs-brom/blob.hex).
Five instruction words matched on the attached board before the warm-reset probe.
The [Cortex-A55 technical reference manual](https://documentation-service.arm.com/static/5e7e1405b471823cb9de57ae)
describes the reset vector and execution-state controls.

## Local compatibility patches

- The FreeRTOS patch adds the missing `memset` declaration and uses 64-bit
  operands for AArch64 system-register reads. It applies only to the generated
  build copy of `port.c`; the downloaded source retains its MIT license notice.
- The A733 SRAM configuration is the `soc_info.c` change from
  [proposal 223](https://github.com/linux-sunxi/sunxi-tools/pull/223), head
  `5d996736a41f975245bd6a22d8d4c3cf044fb38a`.
- FEL status validation comes from [proposal 259](https://github.com/linux-sunxi/sunxi-tools/pull/259),
  head `a90b1b712800068c55e0519368b6a405e0073e9e`.
- The execute-argument patch fixes a locally observed CLI parser error: `exe`
  consumes its command and address, leaving the following command intact.

The FEL patches retain the upstream tool's GPL licensing. The SRAM probe used
the first two FEL patches; the argument-consumption fix was recorded afterward.
DDR and AArch64 probes used all three patches. The DDR wrapper supplies its own
SRAM layout instead of loading the full boot0 through the original SPL path.
