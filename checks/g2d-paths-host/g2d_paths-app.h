/* The host build of apps/g2d-paths/main.c (checks/g2d-paths-host.py): the region indices that the manifest's order
 * gives tools/sunstone-manifest.py's generated header, "surface" then "source". */
#ifndef G2D_PATHS_APP_H
#define G2D_PATHS_APP_H
#include "sunstone_app.h"
void g2d_paths_main(const struct ss_grant *grant);
#define G2D_PATHS_LARGE_SURFACE 0u
#define G2D_PATHS_LARGE_SOURCE 1u
#define G2D_PATHS_LARGE_COUNT 2u
#endif
