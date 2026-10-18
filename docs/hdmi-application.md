# Supervised HDMI colorbar application

The native HDMI application connects shared clock, rail, pin, PMIC and PCK
providers to the HDMI controller, PHY and TV0 timing generator. Its first output
is the hardware colorbar at 1280 × 720, 60 Hz, with limited-range RGB conversion.
The application reads the monitor's EDID and requires the existing controller's
HDMI and mode admission. It does not allocate a framebuffer or enable the display
engine or another DMA master.

Visible output is unqualified. A compatible mini-HDMI cable and monitor are
required for physical observation. Framebuffer-to-HDMI routing remains required;
the existing display-engine implementation currently feeds LCD1 and DSI1.
The [remaining HDMI modes](hdmi.md) include wider timing/color support, audio,
CEC, HDCP and hotplug interrupts. The application does not reduce that scope.

## The image retains ownership through supervised recovery

This is a distinct [supervised RAM-only session](a733-smp-supervised.md).
The watchdog must remain disabled. An immutable session identifies the actual
cold handoff, full analog profile, rail and oscillator admission, PMIC wiring,
remote-owner exclusion and manual-recovery plan. No default analog image or
qualification flag is supplied.

The supervised reservation has a supervision epoch and a zero independent-recovery
epoch. The controller refuses an ambiguous reservation or any supervised mode
other than the limited-range 720p60 pattern. Existing independent-recovery users
retain their default mode. The native configuration and reservation structures
have appended fields, so consumers must rebuild against the new headers; old
binary objects are not compatible.

Boot checks the complete session SHA-256 before trusting provider metadata.
It binds the digest, image build identity, epoch, generation, lease and supervisor
reservation to the supervised launch contract. The session contains an inherited
PMIC configuration with a zero deadline; the application derives the private
provider's absolute deadline from the authenticated boot window.

After the existing all-core startup barrier, the image creates the actual shared
managers and two privileged FreeRTOS tasks. The PMIC task advances read-only
supply observations. The HDMI task waits for that provider, then prepares and
starts the native controller. Resource acquisition and MMIO use the existing
providers; no callback supplies unconditional hardware readiness.

The session lasts twenty seconds from launcher admission. At eighteen seconds,
the application requests a bounded stop, leaving two seconds before the software
deadline. A start that has not reached a stoppable controller state is retained
as a fault. HPD loss, controller errors, clock regression and authority loss also
retain ownership. Repeated fault calls retry retention metadata only.

Stop does not prove output drain. No clock, rail, PCK lease or mutex is released,
and no framebuffer can be reclaimed because none was allocated. An interconnect
stall or another fault can prevent the stop and leave output enabled. Software
deadlines do not provide independent recovery; manual power removal ends the
session. There is no automatic reset, retry, storage write or USB activation.

## Build and prepare the immutable input

The standalone build entry is `apps/hdmi/CMakeLists.txt`; its target is
`sunstone-a733-hdmi`. Supply the pinned FreeRTOS and TinyUSB source directories
and the existing AArch64 toolchain file. TinyUSB is a configuration dependency of
the reused compact-image build definition; it is absent from the linked HDMI
image. Root integration includes `cmake/a733-hdmi-application.cmake` separately.

The native structure in `apps/hdmi/session.h` defines the session ABI. Supply
exactly its linked size as `session.bin`, including zeroed padding. The ELF
exports `a733_hdmi_session_bytes` for that size. IDs must identify real records;
register observations and nonzero digests do not establish physical provenance.

An admission JSON record uses schema `a733-hdmi-supervised-admission-v1`. It names
`image_sha256`, `epoch`, `generation`, `lease` and `supervisor`, and gives paths
for `analog`, `power` and `supervision` records. Relative paths resolve beside
the admission file. The session binds SHA-256 digests of this admission file and
all three records. The coordinator must review those records for the actual board
and linked image before physical launch.

The analog record establishes the complete post-reset PHY image, charge-pump
and reserved/test-field provenance, resistor-control words and inactive output
consumers. The power record establishes the required rails, oscillator,
inherited PMIC bus, bank/pin authority and shared-register exclusion. The
supervision record establishes exclusive board ownership, cold memory admission,
a monitor/cable and the manual-reconnection procedure after a failure.

`apps/hdmi/prepare.py` performs offline packaging:

```sh
python3 apps/hdmi/prepare.py --image /path/to/sunstone-a733-hdmi.elf \
  --session /path/to/session.bin --admission /path/to/admission.json \
  --clang /path/to/clang --lld /path/to/ld.lld --objcopy /path/to/llvm-objcopy \
  --output /path/to/new-bundle
```

It verifies the supplied record identities, preserves their bytes and builds the
existing supervised launcher with the HDMI contract magic. The launcher fills
the absolute boot deadlines; it does not rewrite the session. The bundle lists
payload addresses, reservations and hashes. The packer performs no device access. It snapshots each input once, so later
file replacement cannot change admitted payloads or archived evidence. It checks
record identities and wire encoding; the coordinator must review every ELF
segment, reserved interval and physical-record claim before uploading anything.
A physical launch remains the board coordinator's operation under the existing
lock, admission and one-attempt policy. The current board's failed-instance
record must be resolved before any launch.

## Focused validation covers new application boundaries

`checks/hdmi-application.py` builds the affected native image and checks its linked
providers. The application fixture scripts previously checked leaf completion to
exercise PMIC readiness, start publication, scheduled stop, expired sessions,
health/stop faults, wrong-task refusal and clock regression. Separate cases use
the actual session validator, controller reservation admission and native
constructor to check independent and supervised recovery identities.

The native-constructor cases supply modeled provider metadata and perform no
MMIO. The launcher packing check uses synthetic session bytes only to inspect
its wire encoding. Neither fixture grants physical authority. Existing native
HDMI register-sequence, EDID, PHY, CSC and TV timing evidence is reused rather
than replayed. The native image and reused launcher compile successfully. All seventeen new
application, session and recovery-admission cases pass, together with offline
encoding and input-snapshot checks. No physical operation was attempted.
Preserved evidence is under
`~/.cache/sunstoneos/evidence/hdmi-application-20261017/`.

An optional retained framebuffer image now extends this application. Configure
`SUNSTONE_HDMI_FRAMEBUFFER=ON` for the fixed 720p RGB565 DE1→TV0 path. The default
remains the non-DMA hardware pattern. The framebuffer image requires additional
initial-isolation and memory admission, and retains its complete DMA allocation
through stop or failure. See [the framebuffer image](hdmi-framebuffer.md) for its
memory layout, provider bindings and physical qualification requirements.
