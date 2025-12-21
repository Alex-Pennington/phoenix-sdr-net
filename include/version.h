/**
 * @file version.h
 * @brief Phoenix SDR version information
 *
 * Version format: MAJOR.MINOR.PATCH+BUILD.COMMIT[-dirty]
 * Example: 0.2.5+9.abc1234 or 0.2.5+9.abc1234-dirty
 *
 * Build number increments every build. Commit hash from git.
 */

#ifndef PHOENIX_VERSION_H
#define PHOENIX_VERSION_H

#define PHOENIX_VERSION_MAJOR   3
#define PHOENIX_VERSION_MINOR   0
#define PHOENIX_VERSION_PATCH   0
#define PHOENIX_VERSION_BUILD   1
#define PHOENIX_VERSION_STRING  "3.0.0"
#define PHOENIX_VERSION_FULL    "3.0.0+1.2fa97ce"
#define PHOENIX_GIT_COMMIT      "2fa97ce"
#define PHOENIX_GIT_DIRTY       false

/* Build timestamp - set by compiler */
#define PHOENIX_BUILD_DATE      __DATE__
#define PHOENIX_BUILD_TIME      __TIME__

#include <stdio.h>

static inline void print_version(const char *tool_name) {
    printf("%s v%s (built %s %s)\n",
           tool_name, PHOENIX_VERSION_FULL,
           PHOENIX_BUILD_DATE, PHOENIX_BUILD_TIME);
}

#endif /* PHOENIX_VERSION_H */