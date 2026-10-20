#ifndef SUNSTONE_H264_ENCODE_SNAPSHOT_H
#define SUNSTONE_H264_ENCODE_SNAPSHOT_H
#include "a733_h264_encode_application.h"
#include "boot-record.h"
int h264_encode_session_prepare(const struct a733_smp_contract *);
const struct a733_h264_encode_session *h264_encode_admitted_session(void);
#endif
