#ifndef SUNSTONE_NPU_BOARD_CLIENT_H
#define SUNSTONE_NPU_BOARD_CLIENT_H
/* The NPU runtime client of the board image and its window hand-offs with the
 * USB0 task (docs/designs/npu-board.md, "Window hand-offs").
 *
 * The bootstrap task starts the client once the service is active. The
 * client then sends OPEN itself and publishes ready with a release store
 * after OPEN succeeds; until then USB0 answers every request not-ready and
 * posts nothing. USB0 is the only writer of request.
 *
 * The client task owns the slot's read and write windows. It lends the read
 * window to USB0, which writes the input bytes into it, and after a job the
 * write window, which USB0 transmits in place. USB0 hands a window back with
 * a release store of window and a task notification. The client sends SUBMIT,
 * LOAD or CLOSE only while it holds both windows, and refuses a request that
 * arrives while one is lent. USB0 posts one request at a time and reads the
 * answer with acquire loads. The client writes state, model, job and the
 * tensor lists only while it handles a posted request (or OPEN), before its
 * release store of answer (or ready). Neither task waits on a tick. */
#include "npu_runtime.h"
enum npu_board_window { NPU_BOARD_HELD, NPU_BOARD_READ_LENT, NPU_BOARD_WRITE_LENT };
enum npu_board_operation { NPU_BOARD_LOAD=1, NPU_BOARD_SUBMIT, NPU_BOARD_RETURN, NPU_BOARD_CLOSE };
/* A tensor as USB0 sees it: CPU address and bytes inside a window. */
struct npu_board_tensor { uintptr_t cpu; uint32_t bytes; };
struct npu_board_client {
    struct npu_runtime *rt;
    unsigned slot;
    uint64_t job_timeout_us;
    /* Bound on each runtime request and reply wait; the client waits for
     * notifications without a timeout. */
    TickType_t wait;
    /* The client task, and the bootstrap task it notifies after CLOSE. */
    TaskHandle_t task, bootstrap;
    /* One coherent thermal publication: timestamp in microseconds << 3,
     * ALLOW/ACCEPTED/STOP below. STOP changes timestamp to the first drain
     * deadline and is never cleared. Only the client claims ACCEPTED. */
    atomic_uint_fast64_t admission;
    int (*now)(void *,uint64_t *);
    void *clock;
    uint64_t session_end;
    /* USB0 writes request with release order: sequence<<8 | operation, and
     * argument before it; the client writes answer and the rest. */
    _Alignas(64) atomic_uint request;
    unsigned argument;
    _Alignas(64) atomic_uint answer; /* sequence<<8 | result */
    atomic_uint window, start, ready, closed;
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
/* USB0 only: the client has not finished OPEN; the request had no effect. */
#define NPU_BOARD_RESULT_NOT_READY 4u
/* The client task's loop; returns only after a fault, which it reports. */
void npu_board_client_run(struct npu_board_client *);
/* The bootstrap task, once, after the service is active: the client sends
 * OPEN and then serves USB0's requests. */
void npu_board_client_start(struct npu_board_client *);
/* 1 once OPEN succeeded, with acquire order. */
int npu_board_client_ready(const struct npu_board_client *);
/* USB0: post one request and notify the client. 0 before ready or while the
 * previous request is unanswered. */
int npu_board_client_post(struct npu_board_client *, unsigned sequence, enum npu_board_operation, unsigned argument);
/* USB0: the answer to sequence, or 0 while it is pending. */
unsigned npu_board_client_answer(const struct npu_board_client *, unsigned sequence);
/* USB0: hand a lent window back. */
void npu_board_client_return(struct npu_board_client *);
#define NPU_BOARD_ALLOW UINT64_C(1)
#define NPU_BOARD_ACCEPTED UINT64_C(2)
#define NPU_BOARD_STOP UINT64_C(4)
/* Thermal owner publishes after every policy update, including unchanged
 * NORMAL. until is the oldest channel's freshness deadline; zero cannot
 * admit. stop latches a five-second drain bounded by session_end. */
void npu_board_client_thermal(struct npu_board_client *,int allow,int stop,uint64_t until,uint64_t now);
/* Independent bootstrap supervision. Lost monitoring latches STOP after
 * five seconds without readings. Returns the coherent admission word. */
uint64_t npu_board_client_watch(struct npu_board_client *,uint64_t now);
int npu_board_client_stopping(const struct npu_board_client *);
/* Bootstrap before starting the client, then client before SUBMIT. A claim
 * atomically orders admission against STOP and rechecks freshness after it. */
int npu_board_client_claim(struct npu_board_client *);
void npu_board_client_complete(struct npu_board_client *);
#endif
