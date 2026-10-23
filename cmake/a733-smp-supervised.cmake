# Observer image with supervised admission and non-secure result publication.
if(NOT DEFINED SUNSTONE_SOURCE_ROOT)
  set(SUNSTONE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
if(NOT COMMAND a733_smp_observer)
  include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-smp-observe.cmake")
endif()
set(supervised_root "${SUNSTONE_SOURCE_ROOT}/platform")
a733_smp_observer(a733-smp-supervised "supervised-SU-C3-SU-R1" ORDER continue.S boot-supervised.c
  ADMISSION "${supervised_root}/a733-smp-supervised/admission.c"
    "${supervised_root}/a733-smp-supervised/publication.S"
  PATCHES "boot.c|${supervised_root}/a733-smp-cached/boot.c|platform/a733-smp-observe/boot.patch"
    "continue.S|${supervised_root}/a733-smp-cached/continue.S|platform/a733-smp-observe/continue.patch"
    "boot-supervised.c|boot.c|platform/a733-smp-supervised/boot.patch"
  INCLUDES "${supervised_root}/a733-smp-supervised"
  DEFINITIONS SUNSTONE_SUPERVISED_NS_PUBLICATION=1
    A733_SMP_CONTRACT_MAGIC=0x4153373353554333ULL A733_SMP_RESULT_MAGIC=0x4153373353555231ULL)
