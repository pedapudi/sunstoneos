# Mimus as an external SunstoneOS application

Mimus's `port/sunstoneos-split` branch owns its native application under
`sunstoneos/`: recognizer entry, manifest, WAV capture adapter, USB operations
0–4, transcript decoder, tensor placement, GPU linear kernel, model
preparation and recognition checks. Its engine and formatter remain in the
mimus repository's existing firmware components.

SunstoneOS provides the application kit and generic services. An OS-only
build sets `MIMUS_DIR` and `SUNSTONE_APPLICATION` empty. Supplying `MIMUS_DIR`
and selecting `SUNSTONE_APPLICATION=mimus` registers the external application
through `add_sunstone_application()`; the resulting OS image remains
`sunstone-a733-os.elf`. [Build commands](os-image.md#build-an-os-image) cover
the empty, hello-compute and mimus configurations. Mimus also has a standalone
`sunstoneos/CMakeLists.txt` entry accepting `SUNSTONEOS_DIR`.

## Engine and model

[The optional source record](../apps/mimus/source.json) records the mimus
branch and application commit. The application's own source record pins its
engine baseline. Its build checks the engine input trees and refuses local
changes under `firmware/` or `host/`.

The application retains the P4 all-int8 reference model and punctuation
overlay, exact arithmetic and disabled floating-point contraction. Its
preparation tool changes only Orange Pi target metadata and the corresponding
hash references. Model bodies remain unchanged. INPUT regions in the linked
manifest bind the staged model and recording identities.

## Checks

Run application checks from the mimus checkout with `SUNSTONEOS_DIR` naming
the OS checkout. Host recognition, paced replay, exact GPU sums, peripheral
adapters, native result decoding and QEMU checks live under `sunstoneos/`.
The application's build preserves the existing SMP and native target names.
The OS integration gate invokes the external check paths only when mimus is
explicitly supplied.

## Physical evidence and remaining work

[The retained recognition evidence](mimus-evidence.md) records historical
images, model identities, parity and physical timing. Those measurements do
not qualify the split images. The hardware matrix and handoff retain the
current board and recovery state.

GPU recognition uses the explicitly selected legacy compatibility backend;
the generic queue is not implemented. General CPU INPUT access permissions
are trusted-code obligations rather than enforced per-region protection.
Physical USB/board runs, accessory qualification and recognition performance
qualification remain coordinator work.
