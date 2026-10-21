/* Standalone LGPL-2.0-or-later GStreamer metadata/reader port. */
#ifndef SUNSTONE_HEVC_GST_PORT_H
#define SUNSTONE_HEVC_GST_PORT_H
#include "../h264/gst-port.h"
#define GST_CODEC_PARSERS_API
#define g_return_val_if_fail(x,v) do { if (!(x)) return (v); } while (0)
#define GST_INFO(...) ((void)0)
#define GST_ERROR(...) ((void)0)
#define G_MAXUINT8 UINT8_MAX
#define G_MAXINT16 INT16_MAX
#define G_MININT8 INT8_MIN
#define G_MININT16 INT16_MIN
#define ABS(x) ((x)<0?-(x):(x))
#define div_ceil(x,y) (((x)+(y)-1)/(y))
#endif
