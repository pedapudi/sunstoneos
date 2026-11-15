/* The host build of apps/g2d-rotate/main.c (checks/g2d-rotate-host.py): the region indices that the manifest's order
 * gives tools/sunstone-manifest.py's generated header, "surface" then "source". */
#ifndef G2D_ROTATE_APP_H
#define G2D_ROTATE_APP_H
#include "sunstone_app.h"
void g2d_rotate_main(const struct ss_grant *grant);
#define G2D_ROTATE_LARGE_SURFACE 0u
#define G2D_ROTATE_LARGE_SOURCE 1u
#define G2D_ROTATE_LARGE_COUNT 2u
#endif
