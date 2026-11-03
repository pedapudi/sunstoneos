# Read-only microSD backup probe

A fork of `apps/sd-read-probe`. It identifies the card in profile 2 (four-bit high speed, 50 MHz, 3.3 V), reads the SD Status
register (ACMD13) and decodes the allocation unit both ways (`AU_SIZE` and `UHS_AU_SIZE`), reads one block as the shared plan,
and captures up to 512 listed sectors, one read each, into an image-RAM payload of 272,944 bytes. The SD core is built with
`SUNXI_MMC_READ_ONLY`; the image carries the marker and passes `tools/sd_read_only_scan.py`, so the runner admits it with no
approval record. The image never writes to the card.

Build, with `CMAKE_PROJECT_sunstoneos_INCLUDE` set to `apps/sd-backup-probe/probe.cmake`:

    cmake --build BUILD --target sunstone-a733-sd-backup-probe

Input, run and decode (the ranges are the sectors the bounded write will back up, sector 0 included):

    python3 apps/sd-backup-probe/input.py input --output IN.bin --ranges 0:1 32768:2 62656:128
    python3 tools/board.py submit IMAGE.elf --port 3-1 --kind probe --input IN.bin --worker NAME \
        --hypothesis TEXT --reference docs/sd-card.md \
        --expect EXPECT.json
    python3 apps/sd-backup-probe/input.py decode --report run/output.bin --ranges 0:1 32768:2 62656:128 --backup BACKUP.bin

`decode` writes the backup file (records of a little-endian u64 sector and its 512 bytes, in increasing order) and prints its
SHA-256 with the card registers and the allocation unit. The probe runs at the boot clocks (CPU 494 MHz): the result is
functional, not a performance figure. On failure the report's `reason` names the stop as in `apps/sd-read-probe/README.md`;
16 is the ACMD13 read.
