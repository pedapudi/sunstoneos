#ifndef SUNSTONE_OS_AUDIO_STREAM_H
#define SUNSTONE_OS_AUDIO_STREAM_H
/* I2S0 microphone capture stream of the integrated OS image (apps/os/audio.c).
 *
 * One client opens the stream once, before the scheduler starts, with a
 * format, a period size and a ring of periods in a buffer that it supplies.
 * The DMA service copies its stream profile and catalogue when it starts, so
 * the format and period size stay fixed for the image lifetime. At run time
 * the client starts and stops runs and reads and releases periods. Each run
 * restarts the frame count at zero.
 *
 * The DMA0 interrupt wakes the DMA worker at each period's queue end. The
 * receive DMA writes each period into its own DMA page. Finite mode copies
 * that page into the next client slot before returning it to DMA. Cyclic mode
 * copies the driver's guarded staging lease and never reads raw DMA pages.
 * When every slot is read but not released, the stream drops the
 * completed period instead, and the next delivered period's first frame
 * shows the gap. A stream fault, a discontinuity reported by the DMA service
 * or a stream that stops by itself ends the session through board_fail and
 * keeps every reservation, lease and the stream held.
 *
 * Run-time calls are EL0-safe and belong to one client task. */
#include <stddef.h>
#include <stdint.h>

/* Largest period: one DMA page (A733_I2S_PERIOD_CAPACITY). */
#define OS_AUDIO_PERIOD_CAPACITY 4096u
/* Most ring slots in the client buffer. */
#define OS_AUDIO_RING_CAPACITY 64u
/* Shortest period. The stream task takes one DMA service request per 1 ms
 * kernel tick and each request is answered on its next pass, so a period
 * costs up to four ticks (GET and PUT); four DMA periods absorb delays. A
 * period of at least 8 ms keeps that cost at half the period. With 4,096-byte
 * periods this admits 192 kHz for mono 16-bit samples, and 96 kHz for stereo
 * 16-bit or mono 32-bit samples. */
#define OS_AUDIO_MIN_PERIOD_US 8000u

/* Little-endian samples of bits 16 or 32 in containers of the same size;
 * channels 1 or 2. The wire always carries two 32-bit slots; mono takes
 * slot 0. rate_hz must divide the 24.576 MHz module clock exactly: 8, 12, 16,
 * 24, 32, 48, 64, 96 or 192 kHz. */
struct os_audio_format {
    uint32_t rate_hz;
    uint8_t channels, bits;
    uint8_t reserved[2];
};
struct os_audio_stream_config {
    struct os_audio_format format;
    /* Whole frames, a multiple of 8 sample accesses, at most
     * OS_AUDIO_PERIOD_CAPACITY bytes and at least OS_AUDIO_MIN_PERIOD_US. */
    uint32_t period_bytes;
    /* Ring slots, 2 to OS_AUDIO_RING_CAPACITY. Slot i starts at
     * buffer + i * period_bytes. */
    uint32_t periods;
    /* periods * period_bytes bytes, 4-byte aligned, owned by the client and
     * never moved; only the stream task writes it while a run is active. */
    uint8_t *buffer;
    /* 0 finite periods; 1 hardware-linked RX with guarded staging copies.
     * Physical cyclic use requires measured DMA completion/reuse ordering
     * (docs/i2s-stream.md); choosing this mode establishes no such evidence. */
    unsigned cyclic;
};
/* One delivered period. data lies in the client's buffer and stays valid
 * until the client releases it. The two counter-clock times bracket the
 * stream's observation of the period's DMA completion; they are not wire
 * times. */
struct os_audio_period {
    const uint8_t *data;
    uint32_t bytes, frames;
    uint64_t first_frame;
    uint64_t observed_before_us, observed_after_us;
};

/* Start entry, EL1 before the scheduler: validate the configuration and bind
 * the clock, pins, DMA service and interrupt for it. 1 opened, 0 refused. */
int os_audio_stream_open(const struct os_audio_stream_config *);
/* Start a run once the previous run has stopped. Unreleased periods of the
 * previous run are released. 1 started or already running, 0 the previous
 * run is still stopping, -1 failed. */
int os_audio_stream_start(void);
/* The next period of the current run, in order: 1 delivered, 0 none yet,
 * -1 failed. */
int os_audio_stream_read(struct os_audio_period *);
/* Release the oldest read period of the current run: 1 released, -1 none. */
int os_audio_stream_release(void);
/* Stop the current run: 1 the DMA has drained and the run has stopped,
 * 0 stopping, -1 failed. Periods already read stay valid until the next start
 * or their release. */
int os_audio_stream_stop(void);
#endif
