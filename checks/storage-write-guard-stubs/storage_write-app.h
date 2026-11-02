/* Stand-in for the generated application header (host guard check). */
#ifndef STORAGE_WRITE_APP_H
#define STORAGE_WRITE_APP_H
#include "sunstone_app.h"
void storage_write_main(const struct ss_grant *grant);
#define STORAGE_WRITE_REGION_BOUNDS 0u
#define STORAGE_WRITE_REGION_MODE 1u
#define STORAGE_WRITE_REGION_FLUSH 2u
#endif
