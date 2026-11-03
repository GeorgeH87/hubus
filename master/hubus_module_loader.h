/*
 * Loading master modules as plugins (docs/spec/06-operation.md section 6.10).
 *
 * A plugin is a shared library that exports hubus_module_entry(). The loader refuses a file that other users could
 * have changed, checks the module before it is used (hubus_module_validate()), and refuses one whose name or match
 * rules clash with a module that is already in the set. Modules that are linked statically join the same set.
 */
#ifndef HUBUS_MODULE_LOADER_H
#define HUBUS_MODULE_LOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "hubus_module.h"

#define HUBUS_MODULE_SET_MAX 64u
#define HUBUS_MODULE_NAME_COPY 48u
#define HUBUS_LOAD_DETAIL_LEN 160u

typedef enum {
    HUBUS_LOAD_OK = 0,
    HUBUS_LOAD_FULL,      /* the set holds HUBUS_MODULE_SET_MAX modules */
    HUBUS_LOAD_UNSAFE,    /* the file or directory is writable by others, or belongs to another user */
    HUBUS_LOAD_OPEN,      /* the file cannot be opened as a shared library, or the directory cannot be read */
    HUBUS_LOAD_NO_ENTRY,  /* no hubus_module_entry */
    HUBUS_LOAD_NO_MODULE, /* hubus_module_entry returned NULL */
    HUBUS_LOAD_INVALID,   /* hubus_module_validate() found an error */
    HUBUS_LOAD_DUPLICATE, /* a module with this name is in the set */
    HUBUS_LOAD_CONFLICT   /* match rules as specific as those of a module in the set cover the same hardware */
} hubus_load_status_t;

typedef struct {
    hubus_load_status_t status;
    hubus_module_error_t invalid;          /* INVALID: what is wrong */
    const hubus_module_t *module;          /* OK: the module; DUPLICATE and CONFLICT: the one already in the set */
    char name[HUBUS_MODULE_NAME_COPY];     /* name of the rejected or added module, if it has one */
    char detail[HUBUS_LOAD_DETAIL_LEN];    /* OPEN and UNSAFE: the reason */
} hubus_load_result_t;

typedef struct {
    const hubus_module_t *module[HUBUS_MODULE_SET_MAX];
    void *plugin[HUBUS_MODULE_SET_MAX]; /* the loaded file; NULL: linked statically */
    size_t n;
    bool trust_all; /* skip the owner and permission checks; for tests on file systems without permissions */
} hubus_module_set_t;

/* Called once per file: also for the files that were loaded (status OK). */
typedef void (*hubus_load_report_fn)(void *ctx, const char *path, const hubus_load_result_t *result);

void hubus_module_set_init(hubus_module_set_t *set);

/* Adds a module that is linked statically. */
hubus_load_result_t hubus_module_set_add(hubus_module_set_t *set, const hubus_module_t *module);

/* Loads the file that is at `path` now. A file loaded before that is still in place gives the same module, with the same
 * address, also from another set; a file that was replaced (a new file under the same name, as `install` and `mv` do) gives
 * a new module next to the old one. Do not write into a plugin that is loaded: that changes the code of the running module. */
hubus_load_result_t hubus_module_set_load_file(hubus_module_set_t *set, const char *path);

/* Loads every file ending in ".so" in the directory, in the order of their names. A directory that others could have
 * changed is not read at all. Returns the number of modules added; `report` (may be NULL) gets the result for each file. */
size_t hubus_module_set_load_dir(hubus_module_set_t *set, const char *dir, hubus_load_report_fn report, void *ctx);

/* Closes the plugins. The modules of the set must no longer be used. */
void hubus_module_set_unload(hubus_module_set_t *set);

/* The plugin of entry `i` stays in memory for good: hubus_module_set_unload() does not close it. For a module whose code a
 * thread that cannot be stopped may still run. */
void hubus_module_set_keep(hubus_module_set_t *set, size_t i);

/* True if a file of this owner and mode may be loaded by a process with the effective user `euid`: owned by root or by
 * that user, and not writable by its group or by others. */
bool hubus_load_mode_ok(uid_t owner, mode_t mode, uid_t euid);

const char *hubus_load_status_str(hubus_load_status_t status);
const char *hubus_module_error_str(hubus_module_error_t error);

#endif /* HUBUS_MODULE_LOADER_H */
