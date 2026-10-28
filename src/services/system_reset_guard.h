#ifndef SYSTEM_RESET_GUARD_H
#define SYSTEM_RESET_GUARD_H
#include <stdbool.h>

bool system_reset_busy(void);
bool system_reset_blocks_apps(void);
/* Accepted saves remain writable until reset owns storage. */
bool system_reset_write_allowed(void);

#endif
