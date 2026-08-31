/* wb_cloud.c — cloud project sync with versioning.
 * Local-filesystem backend: stores projects under ~/bigmac_cloud/<name>/
 *   versions/   — version_N.wbus files (one per save, immutable)
 *   meta        — text metadata: version_count, current_version, project_name
 *
 * API: init/save/load/list/delete/get_version_count/restore_version/cleanup.
 * Uses wb_session_save/load for serialization (reuses the existing .wbus
 * text format — human-readable, diffable, robust).
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <libgen.h>
#include "wbus.h"

#ifndef WB_CLOUD_DIR
#define WB_CLOUD_DIR "bigmac_cloud"
#endif

#ifndef WB_MAX_VERSIONS
#define WB_MAX_VERSIONS 256
#endif

/* ---- path helpers ------------------------------------------------------- */

static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    return mkdir(path, 0755);
}

/* Build the cloud root: ~/bigmac_cloud */
static int cloud_root(char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (!home) home = ".";
    int n = snprintf(out, cap, "%s/%s", home, WB_CLOUD_DIR);
    if (n < 0 || (size_t)n >= cap) return -1;
    return ensure_dir(out);
}

/* Build a project's cloud dir: ~/bigmac_cloud/<name>/ */
static int project_dir(const char *project_name, char *out, size_t cap) {
    char root[4096];
    if (cloud_root(root, sizeof(root)) != 0) return -1;
    int n = snprintf(out, cap, "%s/%s", root, project_name);
    if (n < 0 || (size_t)n >= cap) return -1;
    return ensure_dir(out);
}

/* Build the versions dir: ~/bigmac_cloud/<name>/versions/ */
static int versions_dir(const char *project_name, char *out, size_t cap) {
    char pdir[4096];
    if (project_dir(project_name, pdir, sizeof(pdir)) != 0) return -1;
    int n = snprintf(out, cap, "%s/versions", pdir);
    if (n < 0 || (size_t)n >= cap) return -1;
    return ensure_dir(out);
}

/* Build a version file path: ~/bigmac_cloud/<name>/versions/version_N.wbus */
static int version_path(const char *project_name, int version, char *out, size_t cap) {
    char vdir[4096];
    if (versions_dir(project_name, vdir, sizeof(vdir)) != 0) return -1;
    int n = snprintf(out, cap, "%s/version_%d.wbus", vdir, version);
    if (n < 0 || (size_t)n >= cap) return -1;
    return 0;
}

/* Build the meta file path: ~/bigmac_cloud/<name>/meta */
static int meta_path(const char *project_name, char *out, size_t cap) {
    char pdir[4096];
    if (project_dir(project_name, pdir, sizeof(pdir)) != 0) return -1;
    int n = snprintf(out, cap, "%s/meta", pdir);
    if (n < 0 || (size_t)n >= cap) return -1;
    return 0;
}

/* ---- metadata file format ---------------------------------------------- */
/* Plain text, one key=value per line:
 *   project_name=<name>
 *   version_count=<n>
 *   current_version=<n>   (the version last loaded / restored; -1 if none)
 */

static int write_meta(const char *project_name, int version_count, int current_version) {
    char path[4096];
    if (meta_path(project_name, path, sizeof(path)) != 0) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "project_name=%s\n", project_name);
    fprintf(f, "version_count=%d\n", version_count);
    fprintf(f, "current_version=%d\n", current_version);
    fclose(f);
    return 0;
}

static int read_meta(const char *project_name, int *version_count, int *current_version) {
    char path[4096];
    if (meta_path(project_name, path, sizeof(path)) != 0) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    *version_count = 0;
    *current_version = -1;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        /* strip trailing newline */
        char *nl = strchr(val, '\n');
        if (nl) *nl = '\0';
        if (strcmp(key, "version_count") == 0) {
            *version_count = atoi(val);
        } else if (strcmp(key, "current_version") == 0) {
            *current_version = atoi(val);
        }
    }
    fclose(f);
    return 0;
}

/* ---- public API --------------------------------------------------------- */

/* Initialize the cloud storage directory. Returns 0 on success. */
int wb_cloud_init(void) {
    char root[4096];
    return cloud_root(root, sizeof(root));
}

/* Save a new version of a project. Returns the new version number (0-based),
 * or -1 on error. Increments version_count and sets current_version. */
int wb_cloud_save_project(const char *project_name, const wb_session *s) {
    if (!project_name || !s) return -1;

    /* Ensure directories exist */
    char vdir[4096];
    if (versions_dir(project_name, vdir, sizeof(vdir)) != 0) return -1;

    /* Read current meta to determine next version number */
    int version_count = 0, current_version = -1;
    char mpath[4096];
    if (meta_path(project_name, mpath, sizeof(mpath)) == 0) {
        FILE *mf = fopen(mpath, "r");
        if (mf) {
            char line[1024];
            while (fgets(line, sizeof(line), mf)) {
                if (strncmp(line, "version_count=", 14) == 0)
                    version_count = atoi(line + 14);
                else if (strncmp(line, "current_version=", 16) == 0)
                    current_version = atoi(line + 16);
            }
            fclose(mf);
        }
    }

    if (version_count >= WB_MAX_VERSIONS) return -1;

    int new_version = version_count;
    char fpath[4096];
    if (version_path(project_name, new_version, fpath, sizeof(fpath)) != 0) return -1;

    /* Use wb_session_save to serialize the session */
    if (wb_session_save(s, fpath) != 0) return -1;

    /* Update metadata */
    if (write_meta(project_name, version_count + 1, new_version) != 0) return -1;

    return new_version;
}

/* Load a specific version of a project. Returns a new session (caller owns),
 * or NULL on error. Sets *out_version to the loaded version number. */
wb_session *wb_cloud_load_project(const char *project_name, int version, int *out_version) {
    if (!project_name) return NULL;

    /* If version < 0, load the current version */
    int ver = version;
    if (ver < 0) {
        int vc, cv;
        if (read_meta(project_name, &vc, &cv) != 0) return NULL;
        ver = cv;
        if (ver < 0) return NULL;   /* no versions at all */
    }

    char fpath[4096];
    if (version_path(project_name, ver, fpath, sizeof(fpath)) != 0) return NULL;

    /* Check file exists */
    struct stat st;
    if (stat(fpath, &st) != 0) return NULL;

    wb_session *s = wb_session_load(fpath);
    if (!s) return NULL;

    if (out_version) *out_version = ver;
    return s;
}

/* List all projects in the cloud. Fills names_out[0..max_count-1] with
 * project directory names. Returns the number of projects found, capped at
 * max_count. */
int wb_cloud_list_projects(char **names_out, int max_count) {
    if (!names_out || max_count <= 0) return 0;

    char root[4096];
    if (cloud_root(root, sizeof(root)) != 0) return 0;

    DIR *d = opendir(root);
    if (!d) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max_count) {
        if (ent->d_name[0] == '.') continue;   /* skip ., .., hidden */
        /* Check it's a directory */
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", root, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        /* Must have a meta file to be a valid cloud project */
        char mp[4096];
        snprintf(mp, sizeof(mp), "%s/meta", full);
        if (stat(mp, &st) != 0) continue;
        names_out[count] = strdup(ent->d_name);
        if (names_out[count]) count++;
    }
    closedir(d);
    return count;
}

/* Delete a project and all its versions. Returns 0 on success. */
int wb_cloud_delete_project(const char *project_name) {
    if (!project_name) return -1;

    char pdir[4096];
    if (project_dir(project_name, pdir, sizeof(pdir)) != 0) return -1;

    /* Recursively remove the project directory.
     * Simple approach: remove files in versions/, then versions/, then meta, then pdir. */
    char vdir[4096];
    snprintf(vdir, sizeof(vdir), "%s/versions", pdir);
    DIR *d = opendir(vdir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char fp[4096];
            snprintf(fp, sizeof(fp), "%s/%s", vdir, ent->d_name);
            remove(fp);
        }
        closedir(d);
    }
    rmdir(vdir);
    char mp[4096];
    snprintf(mp, sizeof(mp), "%s/meta", pdir);
    remove(mp);
    rmdir(pdir);
    return 0;
}

/* Get the number of versions stored for a project. Returns 0 if not found. */
int wb_cloud_get_version_count(const char *project_name) {
    if (!project_name) return 0;
    int vc = 0, cv = -1;
    if (read_meta(project_name, &vc, &cv) != 0) return 0;
    return vc;
}

/* Restore a specific version as the current version. Copies the version file
 * to a new version slot (so history is preserved). Returns the new version
 * number, or -1 on error. */
int wb_cloud_restore_version(const char *project_name, int version) {
    if (!project_name || version < 0) return -1;

    /* Read meta */
    int vc = 0, cv = -1;
    int mpath_valid = 0;
    char mpath[4096];
    if (meta_path(project_name, mpath, sizeof(mpath)) == 0) {
        mpath_valid = 1;
        FILE *mf = fopen(mpath, "r");
        if (mf) {
            char line[1024];
            while (fgets(line, sizeof(line), mf)) {
                if (strncmp(line, "version_count=", 14) == 0)
                    vc = atoi(line + 14);
                else if (strncmp(line, "current_version=", 16) == 0)
                    cv = atoi(line + 16);
            }
            fclose(mf);
        }
    }

    if (version >= vc) return -1;   /* version doesn't exist */
    if (vc >= WB_MAX_VERSIONS) return -1;

    /* Load the old version */
    char src_path[4096];
    if (version_path(project_name, version, src_path, sizeof(src_path)) != 0) return -1;
    struct stat st;
    if (stat(src_path, &st) != 0) return -1;

    /* Read the file bytes */
    FILE *src = fopen(src_path, "rb");
    if (!src) return -1;
    size_t sz = (size_t)st.st_size;
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(src); return -1; }
    size_t got = fread(buf, 1, sz, src);
    fclose(src);
    if (got != sz) { free(buf); return -1; }

    /* Write to a new version slot */
    int new_version = vc;
    char dst_path[4096];
    if (version_path(project_name, new_version, dst_path, sizeof(dst_path)) != 0) {
        free(buf); return -1;
    }
    FILE *dst = fopen(dst_path, "wb");
    if (!dst) { free(buf); return -1; }
    fwrite(buf, 1, sz, dst);
    fclose(dst);
    free(buf);

    /* Update meta */
    if (mpath_valid) {
        write_meta(project_name, vc + 1, new_version);
    }
    return new_version;
}

/* Remove all but the latest N versions of a project. Keeps versions
 * [version_count - keep_count, version_count - 1]. Returns the number of
 * versions removed, or -1 on error. */
int wb_cloud_cleanup(const char *project_name, int keep_count) {
    if (!project_name || keep_count < 0) return -1;

    int vc = 0, cv = -1;
    if (read_meta(project_name, &vc, &cv) != 0) return -1;

    if (keep_count >= vc) return 0;   /* nothing to remove */

    int remove_count = vc - keep_count;
    int removed = 0;
    for (int i = 0; i < remove_count; i++) {
        char fpath[4096];
        if (version_path(project_name, i, fpath, sizeof(fpath)) != 0) break;
        if (remove(fpath) == 0) removed++;
    }

    /* Renumber remaining versions: versions [remove_count..vc-1] -> [0..vc-remove_count-1] */
    for (int i = remove_count; i < vc; i++) {
        char old_path[4096], new_path[4096];
        if (version_path(project_name, i, old_path, sizeof(old_path)) != 0) break;
        if (version_path(project_name, i - remove_count, new_path, sizeof(new_path)) != 0) break;
        rename(old_path, new_path);
    }

    /* Update meta: version_count -= removed, current_version adjusted */
    int new_vc = vc - removed;
    int new_cv = cv - removed;
    if (new_cv < 0) new_cv = -1;
    write_meta(project_name, new_vc, new_cv);

    return removed;
}