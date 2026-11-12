/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The GLib array types that qemu-plugin.h (QEMU 10.2.1) names. QEMU plugins
 * link against the emulator's own GLib at load time; a plugin that uses only
 * these declarations needs no GLib development headers. */
#ifndef MINIMAL_GLIB_PLUGIN_TYPES
#define MINIMAL_GLIB_PLUGIN_TYPES
typedef struct { char *data; unsigned len; } GArray;
typedef struct { unsigned char *data; unsigned len; } GByteArray;
GByteArray *g_byte_array_new(void);
void g_byte_array_unref(GByteArray *);
void g_array_unref(GArray *);
GByteArray *g_byte_array_set_size(GByteArray *, unsigned);
#endif
