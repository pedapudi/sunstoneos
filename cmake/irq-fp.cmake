# IRQ entry without the FP register save (configSS_IRQ_SAVES_FP 0, platform/a733-smp-cached/FreeRTOSConfig.h).
# Only the gate builds that checks/irq-general-regs.py walks set this option: the check proves that the exception code
# of each image they link uses general registers only. Every other build keeps the save.
option(SUNSTONE_IRQ_FP_UNSAVED "Enter interrupts without saving the FP registers (gate builds checks/irq-general-regs.py walks)" OFF)
if(SUNSTONE_IRQ_FP_UNSAVED)
  add_compile_definitions(SUNSTONE_IRQ_FP_UNSAVED=1)
endif()
