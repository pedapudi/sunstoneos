# The read-only microSD backup probe image; include through CMAKE_PROJECT_sunstoneos_INCLUDE as
# apps/usb-vbus/probe.cmake is. Shared startup, monitor and probe ABI remain the source.
function(sunstone_sd_backup_probe)
  get_target_property(sources a733-kernel-check SOURCES)
  list(REMOVE_ITEM sources checks/kernel.c)
  add_executable(sunstone-a733-sd-backup-probe EXCLUDE_FROM_ALL ${sources}
    apps/sd-backup-probe/main.c apps/sd-backup-probe/region.c apps/sd-backup-probe/el0.S kernel/appmem/app_region.c
    drivers/dram/a733_dram_decode_probe.c drivers/storage/storage_digest.c arch/aarch64/device_memory.c
    drivers/storage/sunxi_mmc.c drivers/storage/fat32_ro.c drivers/storage/a733_mmc_platform.c drivers/storage/a733_mmc_platform_native.c
    platform/a733/shared_locks.c platform/a733/counter_clock.c
    drivers/clock/a733_clock_leases.c drivers/power/a733_rail_leases.c drivers/power/a733_pmic.c
    drivers/power/a733_pmic_native.c drivers/power/a733_pmic_consumer.c
    drivers/pinctrl/a733_pins.c drivers/pinctrl/a733_pin_tables.c drivers/pinctrl/a733_pins_native.c
    drivers/gpu/a733_gpu_resources.c drivers/gpu/a733_gpu_resources_native.c drivers/i2c/a733_i2c.c)
  foreach(property INCLUDE_DIRECTORIES COMPILE_OPTIONS COMPILE_DEFINITIONS LINK_OPTIONS LINK_DEPENDS)
    get_target_property(value a733-kernel-check ${property})
    if(NOT value STREQUAL "value-NOTFOUND")
      set_property(TARGET sunstone-a733-sd-backup-probe PROPERTY ${property} "${value}")
    endif()
  endforeach()
  # SUNXI_MMC_READ_ONLY removes the card write path (checks/sd-read-only.py scans the image).
  target_compile_definitions(sunstone-a733-sd-backup-probe PRIVATE SUNXI_MMC_READ_ONLY=1 SUNSTONE_PLATFORM_UPPER_MAP=1
    INCLUDE_xTaskGetSchedulerState=1 INCLUDE_xTaskGetCurrentTaskHandle=1 INCLUDE_xSemaphoreGetMutexHolder=1)
  target_include_directories(sunstone-a733-sd-backup-probe AFTER PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/sd-backup-probe")
  set_target_properties(sunstone-a733-sd-backup-probe PROPERTIES SUFFIX ".elf")
  include("${SUNSTONE_SOURCE_ROOT}/cmake/sunstone-probe.cmake")
  sunstone_enable_probe(sunstone-a733-sd-backup-probe)
endfunction()
cmake_language(DEFER CALL sunstone_sd_backup_probe)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_FILE}")
