/* The host build of apps/g2d-convert/main.c (checks/g2d-convert-host.py): the region indices that the manifest's order
 * gives tools/sunstone-manifest.py's generated header, "surface", "source" then "reference". */
#ifndef G2D_CONVERT_APP_H
#define G2D_CONVERT_APP_H
#include "sunstone_app.h"
void g2d_convert_main(const struct ss_grant *grant);
#define G2D_CONVERT_LARGE_SURFACE 0u
#define G2D_CONVERT_LARGE_SOURCE 1u
#define G2D_CONVERT_LARGE_REFERENCE 2u
#define G2D_CONVERT_LARGE_COUNT 3u
#endif
