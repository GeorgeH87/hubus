/*
 * Loading master modules as plugins.
 */
#include "hubus_module_loader.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DIR_FILES_MAX 256u
#define NAME_MAX_COPY 128u
#define PLUGIN_SUFFIX ".so"

bool hubus_load_mode_ok(uid_t owner, mode_t mode, uid_t euid)
{
    return (owner == 0 || owner == euid) && (mode & (S_IWGRP | S_IWOTH)) == 0;
}

const char *hubus_load_status_str(hubus_load_status_t status)
{
    switch (status) {
    case HUBUS_LOAD_OK: return "loaded";
    case HUBUS_LOAD_FULL: return "too many modules";
    case HUBUS_LOAD_UNSAFE: return "not safe to load";
    case HUBUS_LOAD_OPEN: return "cannot be opened";
    case HUBUS_LOAD_NO_ENTRY: return "has no hubus_module_entry";
    case HUBUS_LOAD_NO_MODULE: return "offers no module";
    case HUBUS_LOAD_INVALID: return "module is invalid";
    case HUBUS_LOAD_DUPLICATE: return "module name is already in use";
    case HUBUS_LOAD_CONFLICT: return "match rules conflict with another module";
    }
    return "unknown";
}

const char *hubus_module_error_str(hubus_module_error_t error)
{
    switch (error) {
    case HUBUS_MODULE_VALID: return "valid";
    case HUBUS_MODULE_ERR_API: return "other interface version";
    case HUBUS_MODULE_ERR_NAME: return "no name";
    case HUBUS_MODULE_ERR_RULE: return "bad match rule";
    case HUBUS_MODULE_ERR_PROPERTIES: return "bad properties";
    case HUBUS_MODULE_ERR_BLIND: return "bad blind times";
    case HUBUS_MODULE_ERR_ENTITY: return "bad entity";
    case HUBUS_MODULE_ERR_FUNCTIONS: return "bad functions";
    }
    return "unknown";
}

void hubus_module_set_init(hubus_module_set_t *set)
{
    memset(set, 0, sizeof *set);
}

static hubus_load_result_t result_for(hubus_load_status_t status)
{
    hubus_load_result_t r;

    memset(&r, 0, sizeof r);
    r.status = status;
    return r;
}

static void copy_name(hubus_load_result_t *r, const hubus_module_t *m)
{
    if (m && m->name) {
        snprintf(r->name, sizeof r->name, "%s", m->name);
    }
}

static hubus_load_result_t add(hubus_module_set_t *set, const hubus_module_t *m, void *plugin)
{
    hubus_load_result_t r = result_for(HUBUS_LOAD_INVALID);
    size_t i;

    r.invalid = hubus_module_validate(m);
    if (r.invalid != HUBUS_MODULE_VALID) {
        return r;
    }
    copy_name(&r, m);
    if (set->n >= HUBUS_MODULE_SET_MAX) {
        r.status = HUBUS_LOAD_FULL;
        return r;
    }
    for (i = 0; i < set->n; i++) {
        if (strcmp(set->module[i]->name, m->name) == 0) {
            r.status = HUBUS_LOAD_DUPLICATE;
            r.module = set->module[i];
            return r;
        }
    }
    for (i = 0; i < set->n; i++) {
        if (hubus_module_conflicts(set->module[i], m)) {
            r.status = HUBUS_LOAD_CONFLICT;
            r.module = set->module[i];
            return r;
        }
    }
    set->module[set->n] = m;
    set->plugin[set->n] = plugin;
    set->n++;
    r.status = HUBUS_LOAD_OK;
    r.module = m;
    return r;
}

hubus_load_result_t hubus_module_set_add(hubus_module_set_t *set, const hubus_module_t *module)
{
    return add(set, module, NULL);
}

/*
 * The files that are loaded. Under its own name the loader of the system would give back a plugin that is loaded already, even
 * if another file has that name now, so a file is opened and loaded through its descriptor. That name stays with the plugin as
 * long as it is in memory, so the descriptor has to stay open too: a later file must not get the same number. The same file
 * in a second set is the same entry here, without a further dlopen(), which would add a further name.
 */
typedef struct plugin {
    struct plugin *next;
    dev_t dev;
    ino_t ino;
    void *handle;
    const hubus_module_t *module;
    int fd;
    unsigned refs;
} plugin_t;

static plugin_t *plugins;

static plugin_t *plugin_find(const struct stat *st)
{
    plugin_t *p;

    for (p = plugins; p; p = p->next) {
        if (p->dev == st->st_dev && p->ino == st->st_ino) {
            return p;
        }
    }
    return NULL;
}

static void plugin_release(plugin_t *plugin)
{
    plugin_t **link = &plugins;

    if (--plugin->refs != 0u) {
        return;
    }
    while (*link != plugin) {
        link = &(*link)->next;
    }
    *link = plugin->next;
    dlclose(plugin->handle);
    close(plugin->fd);
    free(plugin);
}

/* Loads the open file `fd`, which it takes over on success. Fills r on failure. */
static plugin_t *plugin_open(int fd, const struct stat *st, hubus_load_result_t *r)
{
    const hubus_module_t *(*entry)(void);
    plugin_t *p = malloc(sizeof *p);
    char by_fd[40];
    void *sym;

    if (!p) {
        r->status = HUBUS_LOAD_OPEN;
        snprintf(r->detail, sizeof r->detail, "%s", strerror(ENOMEM));
        return NULL;
    }
    snprintf(by_fd, sizeof by_fd, "/proc/self/fd/%d", fd);
    p->handle = dlopen(by_fd, RTLD_NOW | RTLD_LOCAL);
    if (!p->handle) {
        const char *msg = dlerror();

        r->status = HUBUS_LOAD_OPEN;
        snprintf(r->detail, sizeof r->detail, "%s", msg ? msg : "dlopen failed");
        free(p);
        return NULL;
    }
    sym = dlsym(p->handle, "hubus_module_entry");
    if (!sym) {
        r->status = HUBUS_LOAD_NO_ENTRY;
    } else {
        memcpy(&entry, &sym, sizeof entry);
        p->module = entry();
        if (!p->module) {
            r->status = HUBUS_LOAD_NO_MODULE;
        }
    }
    if (r->status != HUBUS_LOAD_OK) {
        dlclose(p->handle);
        free(p);
        return NULL;
    }
    p->dev = st->st_dev;
    p->ino = st->st_ino;
    p->fd = fd;
    p->refs = 1;
    p->next = plugins;
    plugins = p;
    return p;
}

/* Checks a file or directory; fills r->detail if it is not safe. */
static bool path_safe(const hubus_module_set_t *set, const char *path, bool want_dir, hubus_load_result_t *r)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        r->status = HUBUS_LOAD_OPEN;
        snprintf(r->detail, sizeof r->detail, "%s", strerror(errno));
        return false;
    }
    if (want_dir ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)) {
        r->status = HUBUS_LOAD_UNSAFE;
        snprintf(r->detail, sizeof r->detail, want_dir ? "not a directory" : "not a regular file");
        return false;
    }
    if (!set->trust_all && !hubus_load_mode_ok(st.st_uid, st.st_mode, geteuid())) {
        r->status = HUBUS_LOAD_UNSAFE;
        snprintf(r->detail, sizeof r->detail, "owner %u, mode %04o: only root or the user of the daemon may own it, and "
                 "nobody else may write it", (unsigned)st.st_uid, (unsigned)(st.st_mode & 07777));
        return false;
    }
    return true;
}

hubus_load_result_t hubus_module_set_load_file(hubus_module_set_t *set, const char *path)
{
    hubus_load_result_t r = result_for(HUBUS_LOAD_OK);
    struct stat st;
    plugin_t *p;
    int fd;

    if (!path_safe(set, path, false, &r)) {
        return r;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &st) != 0) {
        r.status = HUBUS_LOAD_OPEN;
        snprintf(r.detail, sizeof r.detail, "%s", strerror(errno));
        if (fd >= 0) {
            close(fd);
        }
        return r;
    }
    p = plugin_find(&st);
    if (p) {
        p->refs++;
        close(fd);
    } else {
        p = plugin_open(fd, &st, &r);
        if (!p) {
            close(fd);
            return r;
        }
    }
    r = add(set, p->module, p);
    if (r.status != HUBUS_LOAD_OK) {
        plugin_release(p); /* r.name is a copy; r.module belongs to the set */
    }
    return r;
}

static int compare_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

size_t hubus_module_set_load_dir(hubus_module_set_t *set, const char *dir, hubus_load_report_fn report, void *ctx)
{
    static const size_t suffix = sizeof PLUGIN_SUFFIX - 1u;
    hubus_load_result_t r = result_for(HUBUS_LOAD_OK);
    char names[DIR_FILES_MAX][NAME_MAX_COPY];
    size_t n = 0, loaded = 0, i;
    struct dirent *e;
    DIR *d;

    if (!path_safe(set, dir, true, &r)) {
        if (report) {
            report(ctx, dir, &r);
        }
        return 0;
    }
    d = opendir(dir);
    if (!d) {
        r.status = HUBUS_LOAD_OPEN;
        snprintf(r.detail, sizeof r.detail, "%s", strerror(errno));
        if (report) {
            report(ctx, dir, &r);
        }
        return 0;
    }
    while ((e = readdir(d)) != NULL && n < DIR_FILES_MAX) {
        size_t len = strlen(e->d_name);

        if (len > suffix && len < NAME_MAX_COPY && strcmp(e->d_name + len - suffix, PLUGIN_SUFFIX) == 0) {
            memcpy(names[n++], e->d_name, len + 1u);
        }
    }
    closedir(d);
    qsort(names, n, sizeof names[0], compare_names);
    for (i = 0; i < n; i++) {
        char path[1024];

        if (snprintf(path, sizeof path, "%s/%s", dir, names[i]) >= (int)sizeof path) {
            continue;
        }
        r = hubus_module_set_load_file(set, path);
        loaded += r.status == HUBUS_LOAD_OK;
        if (report) {
            report(ctx, path, &r);
        }
    }
    return loaded;
}

void hubus_module_set_unload(hubus_module_set_t *set)
{
    size_t i;

    for (i = 0; i < set->n; i++) {
        if (set->plugin[i]) {
            plugin_release(set->plugin[i]);
        }
    }
    hubus_module_set_init(set);
}

void hubus_module_set_keep(hubus_module_set_t *set, size_t i)
{
    set->plugin[i] = NULL; /* its reference is never given back */
}
