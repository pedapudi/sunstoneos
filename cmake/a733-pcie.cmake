# Same supervised board provider composition with one fixed native PCIE task.
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(PCIE)
sunstone_board_application(pcie
 TAG "native-pcie1;bldo1-pd-bank;supervised60;x1gen1-direct-config;final-capability-audit;retained"
 CONTRACT_MAGIC 0x4153373350434331ULL RESULT_MAGIC 0x4153373350435231ULL
 PROFILE pcie-supervised-v1
 SOURCES drivers/pci/a733_pcie.c drivers/pci/a733_pcie_native.c drivers/pci/a733_pcie_phy.c
  apps/pcie/main.c apps/pcie/native.c apps/pcie/run.c apps/pcie/usb0.c apps/pcie/admission.c
  apps/pcie/snapshot.c
 INPUTS include/a733_pcie.h include/a733_pcie_native.h)
