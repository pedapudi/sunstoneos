/* The host build of apps/g2d-check/main.c (checks/g2d-check-host.py): the region indices that the manifest's order
 * gives tools/sunstone-manifest.py's generated header, "surface" then "source". */
#ifndef G2D_CHECK_APP_H
#define G2D_CHECK_APP_H
#include "sunstone_app.h"
void g2d_check_main(const struct ss_grant *grant);
#define G2D_CHECK_LARGE_SURFACE 0u
#define G2D_CHECK_LARGE_SOURCE 1u
#define G2D_CHECK_LARGE_COUNT 2u
#endif
