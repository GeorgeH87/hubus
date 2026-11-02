/*
 * The modules of hubusd: those of the example firmwares (docs/hwid-types.md), linked in, and the plugins of the
 * configured directory.
 */
#ifndef HUBUSD_MODULES_H
#define HUBUSD_MODULES_H

#include <stddef.h>

#include "hubus_module.h"
#include "hubus_module_loader.h"
#include "hubusd_config.h"

const hubus_module_t *const *hubusd_builtin_modules(size_t *count);

/* Adds to the initialised `set` the built-in modules and the plugins of cfg->modules.dir, and reports every plugin file. Returns the
 * number of plugins loaded. The caller closes the set with hubus_module_set_unload(). */
size_t hubusd_load_modules(const hubusd_config_t *cfg, hubus_module_set_t *set, hubus_load_report_fn report, void *ctx);

/* The names Home Assistant knows for the enums of a module. NULL where an enum says "none". */
const char *hubusd_component_name(hubus_component_t component);
const char *hubusd_device_class_name(hubus_device_class_t device_class);
const char *hubusd_unit_text(hubus_unit_t unit);

/* A hubus_load_report_fn that writes to stderr. */
void hubusd_report_module(void *ctx, const char *path, const hubus_load_result_t *r);

#endif /* HUBUSD_MODULES_H */
