# Display timing, connector and panel contracts

A display mode describes an exact pixel clock, active dimensions, front porches,
sync widths, back porches and sync polarity. It does not select a serializer,
framebuffer format, power supply or clock operating point. The refresh helper
returns the pixel clock divided by horizontal and vertical totals as an exact
rational rate. Interlaced modes retain their encoded field geometry and are
refused by the current native output profiles.

The public types and operations are in `include/sunstone_display.h`. HDMI,
DisplayPort, MIPI DSI and parallel LCD have distinct connector kinds. Parallel
panel descriptions can carry a bus width with zero serial lanes. No parallel
output route or native controller adapter is qualified.

## Discovered modes pass a separate hardware check

The Extended Display Identification Data (EDID) parser accepts complete digital
EDID 1.3/1.4 records through eight
128-byte blocks. It checks the advertised extension count, every block checksum,
Consumer Technology Association (CTA) extension boundaries and the separate
DisplayID section checksum. Failure leaves
the caller's mode inventory unchanged. The parser extracts base and CTA detailed
timings, common integer-rate CTA video codes, and DisplayID I/VII detailed timings.
It reports skipped timing encodings, unknown extensions and inventory truncation.
Standard and established timings require an additional Display Monitor Timings
(DMT) table or timing formula;
the parser reports their presence instead of inventing porch values.

Connector operations detect attachment, read complete EDID and validate modes.
Enumeration samples attachment before and after reading. Inventories bind to the
connector and its local generation. A sampled disconnect or provider event change
invalidates an earlier inventory. Providers with interrupt hotplug events supply
an event generation. The HDMI adapter samples HPD and reports no interrupt event
source; pulses entirely between samples remain undetectable. Existing HDMI service
still controls output stop and retained ownership after disconnect. A sampled
disconnect permanently invalidates the owner's cached EDID. A new sink requires
a new admitted owner; reconnect cannot publish an earlier sink's modes.

An explicit mode request never selects a different timing as a fallback. Automatic
selection checks preferred modes first, then the remaining discovered timings.
Admission does not program a clock. The HDMI constructor accepts generic pixel
timing and derives the exact hardware profile key used by the frozen session ABI.
The TV timing controller consumes that pixel timing. HDMI admits its existing
exact 720p60 and 1080p60 analog profiles; an already prepared owner admits only its authenticated
profile. DisplayPort admits its existing 720p60 profile and refuses EDIDs with
more than one extension because its copied-result ABI holds only 256 bytes.
The shared parser itself can handle larger EDIDs.

The DE planner consumes generic pixel timing and either XRGB8888 or RGB565 geometry.
The existing LCD and TV entry points delegate their surface geometry to that
planner. DSI conversion takes explicit HS/LS rates and passes the resulting timing
through the existing PLL, packet and TCON checks. Unsupported geometry, lanes,
polarity, clock factors and transport features receive explicit refusal results.
No new clock or voltage operating point is selected.

## The selected panel remains separate from board wiring

A panel description holds timing modes, lanes or parallel bus width, pixel format,
transport flags, preparation commands, steps after video starts, shutdown commands
and named power, GPIO and backlight resources. The bounded sequence interface
executes one provider step per poll and retains a failure. Providers enforce the
absolute deadline, asynchronous command completion and minimum delays. The
interface does not infer voltages or physical pins from a model name.

The board description records the LCD connector as four-lane DSI1 with no fitted
panel. The DSI application copies that route and adds its authenticated session's
selected panel, exact mode and preparation/enable steps before acquiring supplies.
Native construction checks the panel identity and exact transport timing against
this description. The existing session callback executes panel commands under the
same retained resource ownership. Low-level callers without a board description
retain their existing external authorization requirement.

Two public Linux descriptions exercise the panel interface: Innolux P079ZCA and
LG LH500WX1-SD03. Neither is a default or physically qualified accessory. The
current exact PLL planner refuses the Innolux 56.9 MHz mode; the LCD planner
refuses the LG panel's 1280-line height. These refusals preserve the existing
hardware limits and do not establish that either panel works on this board.

## Host evidence and remaining physical work

no host check checks recorded Dell DP/HDMI, Acer DisplayID and Apple
DisplayID EDIDs, malformed lengths/checksums, generation changes, exact mode
admission, both panel descriptions and bounded panel sequences. Downloaded bytes,
URLs, revisions, SHA-256 values and licenses are recorded in
[the display source manifest](display-class-sources.json). Raw records stay outside
tracked source. Reference-derived legal notices are in
[the notice file](display-panel-MIT.txt).

The existing scanout and native ownership checks cover the composed retained DMA
path. The DSI application fixture exercises supported authenticated 720p and 1080p
sessions through board-route selection, panel description, actual native provider,
PLL, pins, PMIC, IOMMU and scanout code with modeled hardware. Host success does
not establish electrical compatibility, visible pixels, interrupt hotplug delivery,
backlight operation or fault recovery. No panel is attached for physical tests.
