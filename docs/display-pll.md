# Display PLL activation and retuning

SunstoneOS implements the published A733 display PLL 1 enable-and-retune
sequence, with exact clock plans and shared clock ownership. Host comparison
with unchanged source, retained-failure checks and AArch64 compilation pass.
Physical activation remains unqualified because no initial A733 analog profile
has established provenance.

The [PLL API](../include/a733_display_pll.h) accepts an authenticated initial
register image and retunes it to requested high-speed (HS) and low-speed (LS)
output rates.
It can activate a disabled, prepared PLL or retune an already enabled PLL.
The resulting locked state satisfies the clock prerequisite of the native
[DSI PHY preparer](dsi-phy.md). Starting the
[display coordinator](display-scanout.md) from hardware reset still requires
initial analog provisioning and platform integration.

The published A733 Linux and U-Boot paths enable the PLL before changing its
factors. Neither inspected implementation provisions a complete initial PLL
image. The available A733 manual specifies clock limits but omits the display
PLL register map and cold-start sequence. These sources do not establish that
an arbitrary disabled register image can be activated safely.

## The active A733 paths enable before retuning

The Linux reference defines `DISPLL_LINEAR_FREQ` and calls
`displl_clk_enable()` at the start of `displl_clk_set()`. The enable function
performs these field updates in order:

1. Enable the PLL low-dropout regulator through `ldo_en`.
2. Enable the high-speed output gate through `hs_gating`.
3. Enable the low-speed output gate through `ls_gating`.
4. Set `pll_en`.
5. Set `en_lvs`.
6. Enable lock detection through `lockdet_en`.
7. Set `reg_update`.

It then polls the lock bit and waits twenty microseconds. The Linux loop counts
three asserted samples separated by five microseconds, without clearing its
count on a deasserted sample. It also continues after its iteration limit.
A native implementation must require consecutive samples and retain ownership
on timeout. Successful lock observation alone does not qualify output frequency
or analog signaling.

Only after this activation does the Linux rate function write the multiplier,
wait twenty microseconds, write the output dividers and input divider, and set
`reg_update` again. The U-Boot A733 path also selects `DISPLL_LINEAR_FREQ`.
It enables first, writes the dividers, waits twenty microseconds, writes the
multiplier, waits another twenty microseconds, and sets `reg_update` twice.
Its enable helper waits twenty microseconds without polling lock.

The separately published Radxa A733 register source has the same hash as the
Linux register source. It supplies no additional initial analog configuration.

## Native sequencing checks the initial image and every intermediate rate

The runtime borrows the exact common-clock token from prepared media clocks.
It requires exclusive ownership of both the video-output domain and display
PLL 1, with shared oscillator, peripheral-parent and fabric stability.
The registry makes every video-output reservation depend on display PLL 1.
A separate parent writer therefore cannot coexist with that reservation.

The copied reservation binds the token's owner and identity, boot handoff,
inactive-consumer epoch, reference-clock generation and rail generation.
The initial profile binds its own identity and generation to seven complete
PLL words. The platform must authenticate those values against qualified
provenance and live ownership records. Nonzero identifiers alone grant no
hardware authority. Production admission must refuse until a qualified profile
is available; the implementation supplies none.

Before any write, the runtime compares every initial PLL word and checks the
accessible controller enables, output gates and shared clock configuration.
The initial factors must match an exact integer plan with disabled modulation.
The PLL must be fully disabled or fully enabled; partial activation is refused.
The provider must independently establish inactive consumers and exclude remote
writers throughout the operation. Register checks cannot establish those facts
or prove that earlier display transactions have drained.

The planner evaluates every intermediate factor image in the Linux write order.
For example, changing HS/LS from 480/480 MHz to 600/360 MHz has legal endpoints.
Writing the new first LS divider before the second would temporarily produce
1.8 GHz on LS, above its 1.2 GHz limit. The planner refuses that transition
before any callback or hardware access.

The runtime reproduces fourteen source field writes and preserves all other
fields. It checks the live token before each operation, authenticates the
reservation before each write, and compares complete words before and after
masked updates. The common register mutex remains held across yielded waits.
An uncertain write, changed word, lost authority or expired deadline retains
the exclusive reservation and any still-held mutex.

Lock observations must be consecutive and separated by at least five
microseconds. Three successful observations are followed by at least twenty-five
microseconds, preserving the source's final five-microsecond polling delay and
twenty-microsecond settle. The native path adds a second lock filter and final
register verification after retuning. It refuses timeout instead of continuing.

The current readback contract requires the update bit to remain asserted after
its write, as represented by the source model. Physical update-latch behavior
still needs qualification; an unexpected clear retains ownership. The runtime
does not infer a qualified clock from arithmetic, source comparison or lock
readback alone.

Success releases only the register mutex. The exclusive parent token remains
owned. A later atomic registry transition must publish the stable output rates
and replace exclusive PLL change authority with shared consumer ownership.
That publication and integration into the display coordinator remain required
work. Releasing and reacquiring separate tokens would leave an unauthorized
configuration interval and is not an acceptable substitute.

## Factor-first alternatives do not establish the A733 sequence

The Linux register file contains an older
`sunxi_dsi_comb_dphy_pll_set()` function that writes factors and disables
modulation before enabling the PLL. Its call in PHY configuration is commented
out. The active clock framework uses the enable-before-retune functions above.

U-Boot contains a factor-first branch when `DISPLL_LINEAR_FREQ` is absent.
Its preprocessor condition explicitly defines that macro for `SUN60IW2`, the
A733 configuration. A second factor-first implementation appears in the older
`disp2/disp/de/lowlevel_v35x/tcon/de_dsi.c` driver. Its Makefile selects that
directory for `SUN55IW3` and `SUN55IW5`; it does not establish an A733 build.
Matching register field names do not establish matching cold analog behavior.

The register header's default-value comments are also incomplete. They omit
the low-speed divider values and charge-pump field value. They describe enabled
PLL and regulator bits, plus enabled sigma-delta modulation. They cannot serve
as a qualified disabled integer-mode image.

## A complete cold initializer needs initial-state provenance

The A733 manual places the display PLL in the video-output domain. Its source
is the crystal reference, called DCXO in the manual. Its consumers include the
timing controller, LVDS, DSI and combo PHY. The manual specifies a stable
oscillator range of
1.26–2.52 GHz, an actual operating limit of 2.4 GHz, a high-speed output limit
of 1 GHz, and a low-speed output limit of 1.2 GHz.

The existing planner admits exact integer factors within those limits for a
stable 24 MHz reference. This establishes the target arithmetic. It does not
establish the analog state or permissible transient frequencies during startup.

The cold initializer requires evidence for the following conditions:

- **Initial analog state:** documented reset values or an independently traced
  A733 firmware sequence for the PLL analog controls, charge pump, test controls
  and modulation registers. A caller-supplied register image is insufficient
  without a qualified origin.
- **Factor programming:** an A733 sequence that either writes factors while
  disabled or establishes a qualified initial frequency before live retuning.
  The sequence must account for the update latch and its readback semantics.
- **Reference and power:** stable DCXO frequency, video-domain power, relevant
  rails, bus access and reset release. Register values alone cannot establish
  rail stability or reference frequency.
- **All-consumer ownership:** one live reservation covering shared top-level
  selectors, the PLL and every timing-controller, DSI, LVDS and combo-PHY
  consumer affected by those fields. Remote firmware, interrupt handlers and
  concurrent configuration must be excluded throughout activation.
- **Completion and failure:** bounded lock observation, final factor and gate
  verification, and retained ownership after uncertain writes or timeouts.
  Enabling or disabling a PLL must not imply that display DMA has drained.

The implemented sequence supports a prepared initial state whose factors and
analog controls have authenticated provenance. Reset provisioning remains
unresolved and therefore still blocks physical cold initialization.

## Source inspection supplies no physical qualification

Limited inspection of the pinned board boot0 and secure-monitor binaries found
no direct, aligned address constants in the display register range and no
obvious display-PLL routine in the available disassemblies. This does not rule
out computed addresses or initialization in other firmware. Neither binary
currently supplies a traced cold-start sequence.

One AddressSanitizer/UndefinedBehaviorSanitizer session compares complete PHY
register images with unchanged Linux functions at enable, multiplier update
and final retune. It covers disabled and enabled initial states, two rate
transitions, recovery of the lock filter after a low sample, and minimum delays.
The same model uses the real common-clock registry to verify that retained
ownership excludes a second PLL consumer.

Focused cases cover an unauthenticated profile, altered initial words, stale
tokens, busy registry/mutex, uncertain writes, failed readback, late completion
and retained terminal behavior. Planner cases reject missing provenance,
modulation, partial activation and an excessive intermediate LS rate.
These model profiles are synthetic fixtures, not qualified hardware profiles.

The native AArch64 object contains 3,358 bytes of code/read-only data. Direct
stack frames are 288 bytes for planning, 240 for initialization and 48 for
polling; nested planning and callback frames are additional. The object depends
on the existing PLL planner, common-clock token status/retention, `memcpy` and
`memset`.

The first source/model/native batch passed. Evidence and exact executed inputs
are preserved in `~/.cache/sunstoneos/evidence/display-pll-20261016/`.
The final header clarification changes comments only; runtime and check sources
match the successful batch. No board access or performance measurement ran,
and earlier DSI, timing-controller and display-engine checks were not repeated.

Reproduce only when relevant inputs change:

```sh
python3 checks/a733-display-pll.py \
  --source-root ~/.cache/sunstoneos/sources/display \
  --evidence /path/to/new/evidence
```

The inspected sources are:

- [Linux PLL register implementation](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/drm/phy/sunxi_dsi_combophy_reg.c),
  its adjacent register header, and `sunxi_dsi_combophy.c`.
- [U-Boot PLL register implementation](https://github.com/orangepi-xunlong/u-boot-orangepi/blob/b791be842935b27268ae3d00e943a9075495f30a/drivers/video/drm/phys/sunxi_dsi_combophy_reg.c),
  its adjacent register header and PHY driver, and the older display driver's
  [Makefile](https://github.com/orangepi-xunlong/u-boot-orangepi/blob/b791be842935b27268ae3d00e943a9075495f30a/drivers/video/sunxi/disp2/disp/Makefile).
- [Radxa A733 PLL register implementation](https://github.com/radxa/allwinner-bsp/blob/2045a3ca2a01f088c0314dc924bda59d154e363e/drivers/drm/phy/sunxi_dsi_combophy_reg.c)
  and its adjacent register header and PHY driver.
- A733 User Manual V1.00, sections 15.5.5.1–15.5.5.2, printed page 791.
  The pinned PDF SHA-256 is
  `cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`.
- The board boot0 and secure-monitor files pinned in `sources.lock.json` as
  `board-boot0` and `board-monitor`.

Source paths and hashes for the additional U-Boot and Radxa inspection are in
`~/.cache/sunstoneos/sources/display-pll-research/research-inputs.json`.
The next source investigation must resolve the initial analog image and factor
write ordering before a physical cold-start request can be defined.
