#ifndef MODX_VERSION_H
#define MODX_VERSION_H

/* modx-core version (semantic versioning; matches CHANGELOG.md). */
#define MODX_VERSION_MAJOR 0
#define MODX_VERSION_MINOR 2
#define MODX_VERSION_PATCH 0
#define MODX_VERSION "0.2.0"
/* MAJOR * 10000 + MINOR * 100 + PATCH, for #if checks */
#define MODX_VERSION_NUM (MODX_VERSION_MAJOR * 10000 + MODX_VERSION_MINOR * 100 + MODX_VERSION_PATCH)

/* The version the library was built as ("0.2.0"); compare with MODX_VERSION
 * to catch headers and objects from different releases. */
const char *modx_version(void);

#endif
