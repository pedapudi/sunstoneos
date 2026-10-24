#ifndef SUNSTONE_NPU_BOARD_CLIENT_H
#define SUNSTONE_NPU_BOARD_CLIENT_H
/* The NPU runtime client of the board image and its window hand-offs with the
 * USB0 task (docs/designs/npu-board.md, "Window hand-offs").
 *
 * The client task owns the slot's read and write windows. It lends the read
 * window to USB0, which writes the input bytes into it, and after a job the
 * write window, which USB0 transmits in place. USB0 hands a window back with
 * a release store of window and a task notification. The client sends SUBMIT,
 * LOAD or CLOSE only while it holds both windows, and refuses a request that
 * arrives while one is lent. USB0 posts one request at a time and reads the
 * answer with acquire loads; neither task waits on a tick. */
#include "npu_runtime.h"
enum npu_board_window { NPU_BOARD_HELD, NPU_BOARD_READ_LENT, NPU_BOARD_WRITE_LENT };
enum npu_board_operation { NPU_BOARD_OPEN=1, NPU_BOARD_LOAD, NPU_BOARD_SUBMIT, NPU_BOARD_RETURN, NPU_BOARD_CLOSE };
/* A tensor as USB0 sees it: CPU address and bytes inside a window. */
struct npu_board_tensor { uintptr_t cpu; uint32_t bytes; };
struct npu_board_client {
    struct npu_runtime *rt;
    unsigned slot;
    uint64_t job_timeout_us;
    TickType_t wait;
    TaskHandle_t task;
    /* USB0 writes request with release order: sequence<<8 | operation, and
     * argument before it; the client writes answer and the rest. */
    _Alignas(64) atomic_uint request;
    unsigned argument;
    _Alignas(64) atomic_uint answer; /* sequence<<8 | result */
    atomic_uint window;
    unsigned state, model, inputs, outputs;
    uint64_t job;
    struct npu_board_tensor input[NBG_ASSEMBLY_MAX_TENSORS], output[NBG_ASSEMBLY_MAX_TENSORS];
    /* Counts for the host model: runtime requests and replies, and requests
     * refused because a window was lent. */
    unsigned requests, replies, refused;
};
#define NPU_BOARD_RESULT_OK 1u
#define NPU_BOARD_RESULT_REFUSED 2u
#define NPU_BOARD_RESULT_FAULT 3u
/* The client task's loop; returns only after a fault, which it reports. */
void npu_board_client_run(struct npu_board_client *);
/* USB0: post one request and notify the client. 0 while the previous one is
 * unanswered. */
int npu_board_client_post(struct npu_board_client *, unsigned sequence, enum npu_board_operation, unsigned argument);
/* USB0: the answer to sequence, or 0 while it is pending. */
unsigned npu_board_client_answer(const struct npu_board_client *, unsigned sequence);
/* USB0: hand a lent window back. */
void npu_board_client_return(struct npu_board_client *);
#endif
