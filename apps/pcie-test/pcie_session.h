#ifndef SUNSTONE_PCIE_SESSION_H
#define SUNSTONE_PCIE_SESSION_H
#include "a733_board_application.h"
#define A733_PCIE_CONTRACT_MAGIC UINT64_C(0x4153373350544331)
#define A733_PCIE_SESSION_MAGIC UINT64_C(0x4153373350544131)
#define PCIE_OUTPUT_BYTES 4224u
struct pcie_session {
 struct a733_board_session board;
 uint64_t magic,version,bytes,owner,authority,link_timeout_us,pck_client;
 uint8_t initial[32],electrical[32],clocks[32],supervision[32];
 uint8_t inherited[4][3];
 uint16_t vendor,device;
 uint64_t test_timeout_us,bar;
 uint8_t endpoint_test[32];
};
_Static_assert(sizeof(struct pcie_session)==840,"PCIe session geometry");
int pcie_session_valid(const struct pcie_session *);
#endif
