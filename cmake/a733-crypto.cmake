# Same supervised board provider composition with one fixed native CRYPTO task.
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(CRYPTO)
sunstone_board_application(crypto
 TAG "fixed-native-crypto1;supervised60;SHA256-AES128-ECB;exact-private-copy;retained"
 CONTRACT_MAGIC 0x4153373343524331ULL RESULT_MAGIC 0x4153373343525231ULL
 SOURCES drivers/crypto/a733_ce_clock.c apps/crypto/main.c apps/crypto/native.c apps/crypto/run.c
  apps/crypto/request.c apps/crypto/vectors.c apps/crypto/usb0.c apps/crypto/admission.c
  apps/crypto/snapshot.c
 INPUTS apps/crypto/clock-sources.json apps/crypto/inputs.inc apps/crypto/reference.inc)

include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-crypto-single-core.cmake")
