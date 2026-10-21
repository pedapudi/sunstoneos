#ifndef SUNSTONE_PVR_GPU_LIFECYCLE_H
#define SUNSTONE_PVR_GPU_LIFECYCLE_H
/* Optional standalone GPU lifecycle transport. Legacy versions 2/3/4/5 keep
 * their exact byte layouts. All integers below are little endian. No implicit
 * padding or C structure layout is used on the wire. */
#define PVR_GPU_LIFECYCLE_COMPUTE_VERSION 6u
#define PVR_GPU_LIFECYCLE_DRAW_VERSION 7u
#define PVR_GPU_LIFECYCLE_TEXTURE_VERSION 8u
#define PVR_GPU_LIFECYCLE_DEPTH_VERSION 9u
#define PVR_GPU_LIFECYCLE_REQUEST_BYTES 40u
#define PVR_GPU_LIFECYCLE_REPLY_BYTES 48u
#define PVR_GPU_LIFECYCLE_REQUEST_EPOCH 32u
#define PVR_GPU_LIFECYCLE_REPLY_EPOCH 40u
#define PVR_GPU_LIFECYCLE_TEXTURE_REQUEST_BYTES 72u
#define PVR_GPU_LIFECYCLE_TEXTURE_REPLY_BYTES 72u
#define PVR_GPU_LIFECYCLE_TEXTURE_EPOCH 64u
#define PVR_GPU_LIFECYCLE_STATUS 0u
#define PVR_GPU_LIFECYCLE_READ 1u
#define PVR_GPU_LIFECYCLE_SUBMIT 2u
#define PVR_GPU_LIFECYCLE_CLOSE 3u
#define PVR_GPU_LIFECYCLE_DISPOSE 4u
#define PVR_GPU_LIFECYCLE_OPEN 5u
#define PVR_GPU_LIFECYCLE_TEXTURE_DISPOSE 6u
#define PVR_GPU_LIFECYCLE_TEXTURE_OPEN 7u
#define PVR_GPU_LIFECYCLE_PENDING 0u
#define PVR_GPU_LIFECYCLE_READY 1u
#define PVR_GPU_LIFECYCLE_FAULT 2u
#define PVR_GPU_LIFECYCLE_CLOSED 3u
#define PVR_GPU_LIFECYCLE_REFUSED 4u
#define PVR_GPU_LIFECYCLE_DISPOSED 5u
/* STATUS accepts queue epoch zero for discovery. Every other operation binds
 * the exact current epoch; an old queued mutation is refused at consumption.
 * DISPOSE completes only after closed-owner/epoch proof and all child releases;
 * queue epoch stays unchanged. OPEN consumes that proof once, then publishes
 * exactly the next queue epoch. Existing sealed session identity is unchanged.
 * Compute OPEN publishes sequence1 from the sealed input. Render OPEN publishes
 * frame0/generation0 with no readable framebuffer until a fresh submission.
 * All controls carry zero copied words/stream bytes. Texture reply field44 stays
 * closed=1 during DISPOSE and pending OPEN, then0 after successful OPEN. */
#endif
