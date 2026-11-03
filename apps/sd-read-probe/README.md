# Read-only microSD probe

Reads a sequential plan from the microSD card in profile 2 (four-bit high speed,
50 MHz, 3.3 V) with descriptor DMA, times every read, stores up to six single
sectors (the card layout) and returns a binary report. The SD core is built with
`SUNXI_MMC_READ_ONLY`; the runner admits the image only because it passes
`tools/sd_read_only_scan.py`. The image never writes to the card.

Build, with `CMAKE_PROJECT_sunstoneos_INCLUDE` set to `apps/sd-read-probe/probe.cmake`:

    cmake --build BUILD --target sunstone-a733-sd-read-probe

Input and run (the prediction is in
`docs/designs/large-app-regions.md#microsd-read-rate-prediction-before-any-measurement`):

    python3 apps/sd-read-probe/input.py input --output IN.bin --reads 160 --blocks 256 --first 0 \
        --capture 0 1 2 8192 16384 32800
    python3 tools/board.py submit IMAGE.elf --port 3-1 --kind probe --input IN.bin --worker NAME \
        --hypothesis TEXT --reference docs/designs/large-app-regions.md#microsd-read-rate-prediction-before-any-measurement
    python3 apps/sd-read-probe/input.py decode --report run/output.bin

On failure the report's `reason` names the stop:
1 deadline or provider failure, 2 provider construction, 3 identification, 4 card
registers, 5 read submission, 6 read in progress, 7 read result, 8 sector capture,
10 PMIC not parked, 11 card clock stop not confirmed, 12 input, 13 task or provider binding.
