# Supervised board image that reads one 4096-byte block from a direct NVMe
# endpoint through the general PCIe function client, then stops the port.
option(SUNSTONE_A733_PCIE_NVME "Build supervised read-only PCIe NVMe application" OFF)
if(NOT SUNSTONE_A733_PCIE_NVME)
 return()
endif()
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(PCIE_NVME)
sunstone_board_application(pcie-nvme
 TAG "native-pcie-nvme1;function-client;polled-read4096;orderly-stop;retained"
 CONTRACT_MAGIC 0x41533733504e4331ULL RESULT_MAGIC 0x41533733504e5231ULL
 PROFILE pcie-nvme-supervised-v1 WORKLOAD nvme-read-one-block
 SOURCES drivers/pci/a733_pcie.c drivers/pci/a733_pcie_native.c drivers/pci/a733_pcie_phy.c
  drivers/pci/a733_pcie_function.c drivers/pci/a733_pcie_nvme.c
  apps/pcie-nvme/main.c apps/pcie-nvme/native.c apps/pcie-nvme/run.c apps/pcie-nvme/usb0.c
  apps/pcie-nvme/admission.c apps/pcie-nvme/snapshot.c
 DEFINITIONS SUNSTONE_PCIE_FUNCTION=1
 INPUTS include/a733_pcie.h include/a733_pcie_native.h include/a733_pcie_function.h
  include/a733_pcie_nvme.h drivers/pci/source.json)
