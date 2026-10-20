/* Fixed metadata port for LGPL-2.0-or-later GStreamer codec parser sources.
 * Upstream notices remain in the selected source/header. No GLib runtime. */
#ifndef SUNSTONE_H264_GST_PORT_H
#define SUNSTONE_H264_GST_PORT_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint8_t guint8;typedef uint16_t guint16;typedef uint32_t guint32;typedef uint64_t guint64;
typedef int8_t gint8;typedef int16_t gint16;typedef int32_t gint32;typedef int64_t gint64;
typedef unsigned guint;typedef int gint;typedef int gboolean;typedef size_t gsize;
typedef char gchar;typedef void *gpointer;typedef struct GArray GArray;
#define TRUE 1
#define FALSE 0
#define G_BEGIN_DECLS
#define G_END_DECLS
#define GST_USE_UNSTABLE_API 1
#define GST_PADDING 4
#define G_GNUC_INTERNAL
#define G_UNLIKELY(x) (x)
#define G_LIKELY(x) (x)
#define G_N_ELEMENTS(x) (sizeof(x)/sizeof((x)[0]))
#define G_STRINGIFY(x) #x
#define G_MAXUINT16 UINT16_MAX
#define G_MAXINT8 INT8_MAX
#define G_MAXINT32 INT32_MAX
#define G_MAXUINT32 UINT32_MAX
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define GST_DEBUG(...) ((void)0)
#define GST_WARNING(...) ((void)0)
#define GST_LOG(...) ((void)0)
#define g_return_if_fail(x) do { if(!(x)) return; } while(0)
#define g_assert(x) do { if(!(x)) __builtin_trap(); } while(0)
static inline unsigned gst_util_ceil_log2(unsigned v)
{unsigned n=0;if(v) --v;while(v){++n;v>>=1;}return n;}
#endif
