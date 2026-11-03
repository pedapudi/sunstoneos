# Wi-Fi and Bluetooth: historical status record 2026-11-01

This log holds the prose evidence of [the current status](../status/radio.md) as it stood on the date above. It is not updated. Board results that have queue entries are in the records `docs/evidence/radio.jsonl`.

**Status: implemented.**

Native SDC1/CMD52/CMD53 transport, immutable firmware
staging, AIC8800D80 bootstrap, Wi-Fi scan/association/WPA2, IPv4 clients/listener,
DHCP/DNS and reconnect handling have software evidence. Wi-Fi and Bluetooth
share one module owner.

**Board-qualified scope:** None.

Radio identity, firmware loading, RF traffic,
coexistence and headset interoperability remain physically unqualified.

Transport sanitizers, public-record deadlines, dispatcher traffic and selected
OS/radio/headset builds pass under `~/.cache/sunstoneos/build/radio-1031/`.
`radio-startup-facts-1026/`, `radio-os-staging-1026/`,
`radio-bootstrap-rate-1026/` and `bluetooth-startup-deadline-1026/` under
`~/.cache/sunstoneos/evidence/` cover PM-latch initialization, staged inputs
and bounded bootstrap with hardware modeled.

Native identification uses one-bit 400 kHz, and firmware owners require
four-bit 25 MHz. The vendor reference requests SDR104 at 208 MHz, with its
parent arithmetic predicting 200 MHz. Capability reads, CMD5/CMD11 voltage
negotiation, drive/speed selection, PERI1 UHS clocks, CMD19 tuning and INTID194
completion remain missing. A lower bootstrap rate does not satisfy the default
operating-point requirement. [The SDIO comparison](../radio-sdio-reference.md)
records source pins and the complete gap.

Fitted supply topology is blocked: schematic DCDC8 conflicts with DTS
BLDO5/CLDO1. PM2 reaches reset/LPO uses through R61/R1130 with unknown fitted
population; voltage and waveform evidence are absent. Normal OS ownership can
initialize inactive PM latches, but the strict diagnostic also requires complete
authenticated latch history. These contracts do not resolve board wiring.

The identity application retains hardware after success and requires power
removal. Its zero-authority form only refuses before provider construction.
No reusable returning identity image is admitted. See
[qualification](../radio-sdio-qualification.md) and
the identity image (since removed).

Bluetooth implements UART1 H4/HCI, BR/EDR ACL, L2CAP, SDP, RFCOMM, secure headset
admission, HFP/CVSD, optional mSBC playback/capture/duplex and SBC A2DP sink.
Controller credits do not prove headset rendering or wire drain. WPA3 SAE
arithmetic exists; external authentication, protected management frames and SAE
association remain incomplete. SNTP creates RTC SET requests without an image
composition.

Remaining work includes physical entropy and key-state observations, shutdown,
RF drain, sustained receive deadlines, coexistence, broader modes/profiles and
fault recovery. See [platform](../radio-platform.md),
[session](../radio-session.md), [Wi-Fi](../radio-wifi.md),
[Bluetooth](../bluetooth-native.md) and [headset audio](../bluetooth-hfp.md).
