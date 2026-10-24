# The supervised board provider composition with the NPU runtime: one slot,
# the five-model catalogue, the startup sequencer on the power-domain owner
# and INTID 97 on core 1, in the extended-memory profile
# (docs/designs/npu-board.md).
set(SUNSTONE_NPU_BOARD_CATALOGUE "" CACHE FILEPATH "Catalogue artifacts map (name to model path) for the NPU board image")
if(NOT EXISTS "${SUNSTONE_NPU_BOARD_CATALOGUE}")
 message(FATAL_ERROR "The NPU board image requires SUNSTONE_NPU_BOARD_CATALOGUE, the artifacts map of the reviewed catalogue")
endif()
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(NPU_BOARD)
set(npub_build "${CMAKE_CURRENT_BINARY_DIR}/npu-board-application")
file(MAKE_DIRECTORY "${npub_build}")
apply_build_patch("${npub_build}/boot-npu.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/npu-board/boot.patch")
apply_build_patch("${npub_build}/boot.c" "${npub_build}/boot-npu.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/npu-board/extended-boot.patch")
apply_build_patch("${npub_build}/link.ld" "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link-devices.ld"
 "${SUNSTONE_SOURCE_ROOT}/apps/npu-board/extended-link.patch")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${SUNSTONE_SOURCE_ROOT}/apps/npu-board/catalogue.py"
 --artifacts "${SUNSTONE_NPU_BOARD_CATALOGUE}" --output "${npub_build}/models.c" COMMAND_ERROR_IS_FATAL ANY)
sunstone_board_application(npu-board BOOT "${npub_build}/boot.c" LINK_SCRIPT "${npub_build}/link.ld"
 BUILD_DIR "${npub_build}"
 TAG "runtime-native-npu-board1;supervised20;five-model-catalogue;one-slot;intid97-core1;extended-WB"
 PROFILE npu-board-supervised-v1
 CONTRACT_MAGIC 0x415337334e424331ULL RESULT_MAGIC 0x415337334e425231ULL
 DEFINITIONS SUNSTONE_A733_EXTENDED_DEVICE_MEMORY=1 SUNSTONE_A733_IRQ_TABLE=1 A733_BOARD_SUPERVISED_SECONDS=20
 INCLUDES apps/npu-board platform/a733-smp-cached
 SOURCES apps/npu-board/main.c apps/npu-board/client.c apps/npu-board/irq.c apps/npu-board/usb.c
  apps/npu-board/admission.c apps/npu-board/snapshot.c "${npub_build}/models.c"
  platform/a733/device_memory_layout.c platform/a733-smp-cached/irq_table.c drivers/gpu/gicv3_spi.c
  platform/a733/npu_native.c runtime/counter_clock.c drivers/npu/a733_npu_runtime.c drivers/npu/a733_npu_runtime_interim.c
  drivers/npu/npu_service.c drivers/npu/a733_npu_controller.c drivers/npu/a733_npu_memory.c
  drivers/npu/a733_npu_mmu.c drivers/npu/nbg_model.c drivers/npu/nbg_assembly.c
  drivers/npu/nbg_checksum.c drivers/npu/nbg_relocation.c drivers/npu/a733_npu_startup.c
  drivers/npu/a733_npu_platform.c drivers/npu/a733_npu_platform_native.c drivers/npu/a733_npu_pll.c
 INPUTS apps/npu-board/boot.patch apps/npu-board/extended-boot.patch apps/npu-board/extended-link.patch
  apps/npu-board/catalogue.py tools/npu-session-models.json platform/a733-smp-cached/link-devices.ld
  "${SUNSTONE_NPU_BOARD_CATALOGUE}" include/npu_runtime.h include/a733_npu_startup.h)
