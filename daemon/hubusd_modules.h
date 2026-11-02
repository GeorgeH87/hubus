/*
 * The modules of the example firmwares (docs/hwid-types.md), linked into hubusd.
 */
#ifndef HUBUSD_MODULES_H
#define HUBUSD_MODULES_H

#include <stddef.h>

#include "hubus_module.h"

const hubus_module_t *const *hubusd_builtin_modules(size_t *count);

#endif /* HUBUSD_MODULES_H */
