#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "renamer.h"

static void print_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s <folder> <ext> <prefix> <padding>\n", program_name);
}

static int parse_padding(const char *arg, int *padding_out) {
    char *endptr = NULL;
    long parsed = strtol(arg, &endptr, 10);

    if (arg[0] == '\0' || *endptr != '\0') {
        return -1;
    }

    if (parsed <= 0 || parsed > INT_MAX) {
        return -1;
    }

    *padding_out = (int)parsed;
    return 0;
}

int main(int argc, char **argv) {
    const char *folder;
    const char *ext;
    const char *prefix;
    int padding;
    char **files = NULL;
    size_t file_count = 0;
    int rc;

    // Expect exactly 4 user arguments (program name + 4)
    if (argc != 5) {
        print_usage(argv[0]);
        return 1;
    }

    folder = argv[1];
    ext = argv[2];
    prefix = argv[3];

    if (ext[0] != '.' || ext[1] == '\0') {
        fprintf(stderr, "Extension must start with '.' and contain at least one character.\n");
        return 1;
    }

    if (parse_padding(argv[4], &padding) != 0) {
        fprintf(stderr, "Padding must be a positive integer.\n");
        return 1;
    }

    // Collect filenames in `folder` that end with `ext`.
    // Returns an allocated `char **` array and the number of entries.
    rc = collect_matching_files(folder, ext, &files, &file_count);
    if (rc != 0) {
        return 1;
    }

    if (file_count == 0) {
        printf("No files with extension %s found in %s.\n", ext, folder);
        return 0;
    }

    // Sort filenames deterministically (alphabetical) so renames are predictable.
    sort_files(files, file_count);

    // Build new names with zero-padded numbers and perform the renames.
    rc = perform_renames(folder, ext, prefix, padding, files, file_count);
    free_file_list(files, file_count);

    if (rc != 0) {
        return 1;
    }

    return 0;
}
