/* FatFs R0.16 configuration for the read-only microSD volume. The option list
 * follows the pinned ffconf.h template; docs/filesystem.md explains choices. */
#ifndef SUNSTONE_FFCONF_H
#define SUNSTONE_FFCONF_H
/* The pinned ff.h defines UINT as size_t without including <stddef.h>. */
#include <stddef.h>

#define FFCONF_DEF	80386	/* Revision ID of the pinned R0.16 sources. */

/* Function configuration: read-only, no formatting or string helpers. */
#define FF_FS_READONLY	1
#define FF_FS_MINIMIZE	0
#define FF_USE_FIND	0
#define FF_USE_MKFS	0
#define FF_USE_FASTSEEK	0
#define FF_USE_EXPAND	0
#define FF_USE_CHMOD	0
#define FF_USE_LABEL	0
#define FF_USE_FORWARD	0
#define FF_USE_STRFUNC	0
#define FF_PRINT_LLI	0
#define FF_PRINT_FLOAT	0
#define FF_STRF_ENCODE	3

/* Names: U.S. OEM code page for short names, static long-name buffers (one
 * task owns the volume) and UTF-8 path strings. exFAT requires long names. */
#define FF_CODE_PAGE	437
#define FF_USE_LFN	1
#define FF_MAX_LFN	255
#define FF_LFN_UNICODE	2
#define FF_LFN_BUF	255
#define FF_SFN_BUF	12
#define FF_FS_RPATH	0
#define FF_PATH_DEPTH	10

/* One volume on physical drive 0: an unpartitioned card or the first FAT or
 * exFAT partition in its MBR. GPT needs FF_LBA64 and is not admitted. */
#define FF_VOLUMES	1
#define FF_STR_VOLUME_ID	0
#define FF_VOLUME_STRS	"SD"
#define FF_MULTI_PARTITION	0
#define FF_MIN_SS	512
#define FF_MAX_SS	512
#define FF_LBA64	0
#define FF_MIN_GPT	0x10000000
#define FF_USE_TRIM	0

/* System configuration. Each open file keeps its own sector buffer. */
#define FF_FS_TINY	0
#define FF_FS_EXFAT	1
#define FF_FS_NORTC	1
#define FF_NORTC_MON	1
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2025
#define FF_FS_CRTIME	0
#define FF_FS_NOFSINFO	0
#define FF_FS_LOCK	0
#define FF_FS_REENTRANT	0
#define FF_FS_TIMEOUT	1000
/* ESP-IDF option in the pinned sources: fixed in-object sector buffers. */
#define FF_USE_DYN_BUFFER	0

/* Keep the FatFs media interface out of the global disk_* namespace. */
#define disk_initialize	fatfs_port_disk_initialize
#define disk_status	fatfs_port_disk_status
#define disk_read	fatfs_port_disk_read
#define disk_write	fatfs_port_disk_write
#define disk_ioctl	fatfs_port_disk_ioctl

#endif
