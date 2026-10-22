#ifndef SUNSTONE_PCIE_SESSION_H
#define SUNSTONE_PCIE_SESSION_H
#include "a733_board_application.h"
#define A733_PCIE_CONTRACT_MAGIC UINT64_C(0x41533733504e4331)
#define A733_PCIE_SESSION_MAGIC UINT64_C(0x41533733504e4131)
/* 192-byte copied identity record, then the 4096 bytes read from the namespace. */
#define PCIE_OUTPUT_BYTES 4288u
struct pcie_session {
 struct a733_board_session board;
 uint64_t magic,version,bytes,owner,authority,link_timeout_us,pck_client;
 uint8_t initial[32],electrical[32],clocks[32],supervision[32];
 uint8_t inherited[4][3];
 uint16_t vendor,device;
 /* Bound for each NVMe command and for the controller shutdown. */
 uint64_t command_timeout_us,lba;
 uint32_t function,namespace_id;
 uint8_t nvme[32];
};
_Static_assert(sizeof(struct pcie_session)==848,"PCIe NVMe session geometry");
int pcie_session_valid(const struct pcie_session *);
#endif
