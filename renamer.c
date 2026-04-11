#include "renamer.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define INITIAL_CAPACITY 16
#define NAME_BUFFER_SIZE 1024
#define PATH_BUFFER_SIZE 4096

static int compare_names(const void *left, const void *right) {
    const char *const *lhs = (const char *const *)left;
    const char *const *rhs = (const char *const *)right;
    return strcmp(*lhs, *rhs);
}

static char *duplicate_string(const char *source) {
    size_t len = strlen(source);
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
        return NULL;
    }

    memcpy(copy, source, len + 1);
    return copy;
}

static int build_path(char *out, size_t out_size, const char *folder, const char *name) {
    size_t folder_len = strlen(folder);
    const char *separator = "";
    int written;

    if (folder_len > 0) {
        char last = folder[folder_len - 1];
        if (last != '/' && last != '\\') {
            separator = "/";
        }
    }

    written = snprintf(out, out_size, "%s%s%s", folder, separator, name);
    if (written < 0 || (size_t)written >= out_size) {
        return -1;
    }

    return 0;
}

static int is_regular_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }

    return S_ISREG(st.st_mode);
}

static int find_name_index(char **names, size_t count, const char *name) {
    size_t i;
    for (i = 0; i < count; i++) {
        if (strcmp(names[i], name) == 0) {
            return (int)i;
        }
    }

    return -1;
}

int str_ends_with(const char *str, const char *suffix) {
    size_t str_len;
    size_t suffix_len;

    if (!str || !suffix) {
        return 0;
    }

    str_len = strlen(str);
    suffix_len = strlen(suffix);

    if (suffix_len > str_len) {
        return 0;
    }

    return strcmp(str + (str_len - suffix_len), suffix) == 0;
}

int collect_matching_files(const char *folder, const char *ext, char ***files_out, size_t *count_out) {
    DIR *dir;
    struct dirent *entry;
    char **files = NULL;
    size_t count = 0;
    size_t capacity = 0;
    int status = -1;

    if (!folder || !ext || !files_out || !count_out) {
        return -1;
    }

    dir = opendir(folder);
    if (!dir) {
        perror("opendir");
        return -1;
    }

    /*
     * Scan directory entries and collect names that end with `ext`.
     * Skip dot-prefixed (hidden) entries and non-regular files.
     */
    while (1) {
        char full_path[PATH_BUFFER_SIZE];
        char *name_copy;
        char **grown;

        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            if (errno != 0) {
                perror("readdir");
                goto cleanup;
            }
            break;
        }

        // Skip hidden files and directory entries starting with a dot
        if (entry->d_name[0] == '.') {
            continue;
        }

        if (!str_ends_with(entry->d_name, ext)) {
            continue;
        }

        if (build_path(full_path, sizeof(full_path), folder, entry->d_name) != 0) {
            fprintf(stderr, "Path too long while scanning: %s\n", entry->d_name);
            goto cleanup;
        }

        if (!is_regular_file(full_path)) {
            continue;
        }

        if (count == capacity) {
            size_t new_capacity = (capacity == 0) ? INITIAL_CAPACITY : capacity * 2;
            grown = (char **)realloc(files, new_capacity * sizeof(char *));
            if (!grown) {
                perror("realloc");
                goto cleanup;
            }
            files = grown;
            capacity = new_capacity;
        }

        name_copy = duplicate_string(entry->d_name);
        if (!name_copy) {
            perror("malloc");
            goto cleanup;
        }

        files[count++] = name_copy;
    }

    *files_out = files;
    *count_out = count;
    files = NULL;
    status = 0;

cleanup:
    free_file_list(files, count);

    if (closedir(dir) != 0) {
        perror("closedir");
        status = -1;
    }

    return status;
}

void sort_files(char **files, size_t count) {
    if (!files || count < 2) {
        return;
    }

    qsort(files, count, sizeof(char *), compare_names);
}

int perform_renames(const char *folder, const char *ext, const char *prefix, int padding,
                    char **files, size_t count) {
    /*
     * Pre-generate all target basenames (prefix + zero-padded number + ext)
     * so we can detect conflicts before performing any filesystem changes.
     */
    char **new_names;
    size_t i;

    if (!folder || !ext || !prefix || !files) {
        return -1;
    }

    new_names = (char **)malloc(count * sizeof(char *));
    if (!new_names) {
        perror("malloc");
        return -1;
    }

    for (i = 0; i < count; i++) {
        char generated_name[NAME_BUFFER_SIZE];
        int written = snprintf(generated_name, sizeof(generated_name), "%s%0*d%s", prefix, padding,
                               (int)(i + 1), ext);

        if (written < 0 || (size_t)written >= sizeof(generated_name)) {
            fprintf(stderr, "Generated name is too long for index %zu.\n", i + 1);
            free_file_list(new_names, i);
            return -1;
        }

        new_names[i] = duplicate_string(generated_name);
        if (!new_names[i]) {
            perror("malloc");
            free_file_list(new_names, i);
            return -1;
        }
    }

    /*
     * Preflight check: ensure no target path already exists on disk (or is
     * also present in the source list). Abort early to avoid partial renames.
     */
    for (i = 0; i < count; i++) {
        char new_path[PATH_BUFFER_SIZE];
        struct stat st;
        int source_index;

        if (strcmp(files[i], new_names[i]) == 0) {
            continue;
        }

        if (build_path(new_path, sizeof(new_path), folder, new_names[i]) != 0) {
            fprintf(stderr, "Path too long while checking target: %s\n", new_names[i]);
            free_file_list(new_names, count);
            return -1;
        }

        if (stat(new_path, &st) == 0) {
            source_index = find_name_index(files, count, new_names[i]);
            if (source_index >= 0) {
                fprintf(stderr,
                        "Rename conflict: target %s is also an existing source file. "
                        "Use a different prefix or clear existing names first.\n",
                        new_names[i]);
            } else {
                fprintf(stderr, "Rename conflict: target already exists: %s\n", new_names[i]);
            }
            free_file_list(new_names, count);
            return -1;
        }
    }

    /* Perform the actual renames now that preflight checks passed. */
    for (i = 0; i < count; i++) {
        char old_path[PATH_BUFFER_SIZE];
        char new_path[PATH_BUFFER_SIZE];

        if (strcmp(files[i], new_names[i]) == 0) {
            continue;
        }

        if (build_path(old_path, sizeof(old_path), folder, files[i]) != 0 ||
            build_path(new_path, sizeof(new_path), folder, new_names[i]) != 0) {
            fprintf(stderr, "Path too long while renaming: %s\n", files[i]);
            free_file_list(new_names, count);
            return -1;
        }

        if (rename(old_path, new_path) != 0) {
            fprintf(stderr, "Failed to rename %s to %s: %s\n", files[i], new_names[i], strerror(errno));
            free_file_list(new_names, count);
            return -1;
        }

        printf("%s -> %s\n", files[i], new_names[i]);
    }

    free_file_list(new_names, count);
    return 0;
}

void free_file_list(char **files, size_t count) {
    size_t i;

    if (!files) {
        return;
    }

    for (i = 0; i < count; i++) {
        free(files[i]);
    }

    free(files);
}
