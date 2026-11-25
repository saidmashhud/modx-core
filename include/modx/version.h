#ifndef MODX_VERSION_H
#define MODX_VERSION_H

/**
 * @file version.h
 * @brief modx-core version (semantic versioning; matches CHANGELOG.md).
 */
#define MODX_VERSION_MAJOR 0      /**< Major version. */
#define MODX_VERSION_MINOR 2      /**< Minor version. */
#define MODX_VERSION_PATCH 0      /**< Patch version. */
#define MODX_VERSION "0.2.0"      /**< Version string of these headers. */
/** MAJOR * 10000 + MINOR * 100 + PATCH, for #if checks. */
#define MODX_VERSION_NUM (MODX_VERSION_MAJOR * 10000 + MODX_VERSION_MINOR * 100 + MODX_VERSION_PATCH)

/** The version the library was built as ("0.2.0"); compare with MODX_VERSION
 * to catch headers and objects from different releases. */
const char *modx_version(void);

#endif
