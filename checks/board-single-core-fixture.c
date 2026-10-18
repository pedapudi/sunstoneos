/* Software fixture only. Configuration fields follow the accepted CE,
 * radio-platform and radio-session producers; evidence IDs grant no authority. */
#include "radio.h"
const struct board_radio_profile board_single_core_radio_fixture={
 .magic=BOARD_RADIO_PROFILE_MAGIC,.version=BOARD_RADIO_PROFILE_SUPERVISED_VERSION,.bytes=sizeof(struct board_radio_profile),
 .platform_authority=301,.ce_authority=302,.secure_ce_authority=303,.entropy_record=304,
 .characterization_record=305,.startup_test_record=306,.health_enable_epoch=307,
 .chip_id={0x11223344,0x55667788,0x99aabbcc,0xddeeff00},
 .platform_evidence={1},.ce_evidence={2},.entropy_evidence={3},
 .platform={.owner=42,.topology={201,A733_RADIO_DCDC1,A733_RADIO_DCDC8,A733_RADIO_DCDC8,A733_RADIO_DCDC8},
  .fabric={0,1u<<31,1u<<31},.fabric_hz={24000000,24000000,24000000},
  .desired={{2,0,0},{2,0,0},{2,0,0},{2,0,0},{2,0,0},{2,0,0},{0,0,0},{1,0,0}},
  .card={.supported_ocr=0x00300000,.command_timeout=20000,.ready_poll_interval=100}},
 .ce={.owner=43,.health_profile=8,.fabric={0,1u<<31,1u<<31},.fabric_hz={24000000,24000000,24000000},
  .health={9,325,0,0x30200},.profile=A733_CE_TRNG_ONLY},
 .firmware_address={0x43010000,0x43011000,0x43018000,0x43020000},
 .firmware_bytes={1708,24744,23288,341260},
 .radio_identity={.chip_word=0x00c30000,.subrevision_word=0x70},
 .transport={.variant=AIC8800_TRANSPORT_D80_BOOTSTRAP,.pad=AIC8800_PAD_KEEP,.ticks_per_ms=1000,.command_timeout=100000,.poll_interval=1000},
 .bootstrap={1,0x40500058,0x40,1,1},
 .wifi={.rf_profile_record=1,.channel_policy_record=2,.command_timeout=1000000,.profile=1,.channel_count=1,.channels={{2412,0,1,17}}},
 .target={.security=AIC8800_WIFI_WPA2_PSK_CCMP,.beacon_evidence=20},
 .session_owner=44,.runtime_lease_us=10000000,.renew_margin_us=3000000,
 .binding_timeout_us=1000000,.command_timeout_us=100000,.refill_timeout_us=100000,
 .tx_timeout_us=100000,.association_timeout_us=1000000,.security_timeout_us=1000000,
 .queue_timeout_ticks=5,.client_timeout_ticks=5000,.peer_port=80,.request_bytes=4,.response_bytes=5,
 .peer_address={10,0,0,1},.request={'p','i','n','g'},.response={'p','o','n','g','!'},
 .address={10,0,0,2},.netmask={255,255,255,0},.gateway={10,0,0,1}
};
