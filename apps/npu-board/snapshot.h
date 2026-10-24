#ifndef SUNSTONE_NPU_BOARD_SNAPSHOT_H
#define SUNSTONE_NPU_BOARD_SNAPSHOT_H
#include "npu-board-session.h"
#include "boot-record.h"
int npu_board_session_prepare(const struct a733_smp_contract *);
const struct npu_board_session *npu_board_admitted_session(void);
#endif
