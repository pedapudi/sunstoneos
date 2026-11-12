# Compare captured HEVC pictures with a CPU decoder

The native HEVC application decodes a 640-by-480 Main-profile IDR picture
and a P picture. Its host capture saves each copied picture as NV12, with
307,200 luma bytes followed by 153,600 interleaved chroma bytes.
Each file contains 460,800 bytes.

Run the offline comparator after capture, using the frozen input clip and
the two saved pictures:

```sh
~/.cache/sunstoneos/venv/bin/python3 -B tools/hevc-pixel-reference.py \
  --clip /path/to/frozen/input.h265 \
  --picture0 /path/to/capture/picture-0.nv12 \
  --picture1 /path/to/capture/picture-1.nv12 \
  --output ~/.cache/sunstoneos/build/video-1031/pixel-comparison
```

Replace the input paths with the selected run's files. The output directory
must be new. The command reads local files and accesses no board or network.
It freezes the bytes it compares and retains the reference, commands, tool
versions, hashes and diagnostic logs in that directory.

The comparator requires one Main-profile, 8-bit 640-by-480 video stream.
FFmpeg's software HEVC decoder runs on one host CPU thread with hardware
acceleration disabled. Passthrough timing prevents frame duplication or
dropping. The complete clip must decode to two pictures; extra output,
decode errors and incomplete copies fail the check.

Success requires every Y, U and V sample in both pictures to match.
`result.json` records `pixels_equal`, per-picture hashes and per-plane
differences. A mismatch returns a nonzero exit code and preserves its first
differing offset and maximum absolute error. Short copies, invalid streams
and tool failures also return a nonzero exit code with a retained report.

Before using equality as board evidence, match the report's clip and copy
hashes to the authenticated capture record. Keep the run log, image hash
and source revision with that record. The comparator always reports
`physical_qualified: false`: it does not establish native execution, IRQ
delivery, DMA confinement, memory visibility, drain or recovery.

## Check the comparator with altered CPU output

```sh
~/.cache/sunstoneos/venv/bin/python3 -B checks/hevc-pixel-reference-host.py \
  --clip ~/.cache/sunstoneos/evidence/hevc-decode-20261020/two-pictures.hevc \
  --output ~/.cache/sunstoneos/build/video-1031/pixel-comparator-check
```

The host check uses CPU-decoded copies. It verifies equality, altered Y/U/V
samples in each picture, swapped pictures, extent errors, extra decoded
pictures, wrong geometry, failed probing and CLI failure reports.
Those results validate the comparison tool and supply no native pixel evidence.

## FFmpeg executes only on the host

[The source record](pixel-reference-sources.json) identifies the installed
reference tools, their source package and license. FFmpeg source is available;
no FFmpeg library or executable is linked into the native image.
Every comparison records the selected executable hashes and version strings.
Use `--ffmpeg` and `--ffprobe` to select another installed reference build;
retain its source and license record with the comparison evidence.
