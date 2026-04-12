#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "renamer.h"

#define DEFAULT_HISTORY_FILE ".rename_history"

static void print_usage(const char *program_name) {
    fprintf(stderr,
            "Usage:\n"
            "  %s [options] <folder> <ext> <prefix> <padding>\n"
            "  %s [options] --regex <pattern> <folder> <prefix> <padding>\n"
            "  %s undo [--yes] [history-file]\n"
            "\n"
            "Options:\n"
            "  --dryrun           Show planned renames without touching files.\n"
            "  --yes              Skip confirmation prompts.\n"
            "  -r, --recursive    Process subfolders recursively.\n"
            "  --sort <mode>      Sort mode: name, ctime, size.\n"
            "  --regex <pattern>  Regex filter (mutually exclusive with extension).\n",
            program_name, program_name, program_name);
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

static int parse_sort_mode(const char *arg, SortMode *sort_mode_out) {
    if (strcmp(arg, "name") == 0) {
        *sort_mode_out = SORT_MODE_NAME;
        return 0;
    }

    if (strcmp(arg, "ctime") == 0) {
        *sort_mode_out = SORT_MODE_CTIME;
        return 0;
    }

    if (strcmp(arg, "size") == 0) {
        *sort_mode_out = SORT_MODE_SIZE;
        return 0;
    }

    return -1;
}

static int parse_undo_args(int argc, char **argv, const char **history_file_out, int *assume_yes_out) {
    const char *history_file = DEFAULT_HISTORY_FILE;
    int history_set = 0;
    int i;

    for (i = 2; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--yes") == 0) {
            *assume_yes_out = 1;
            continue;
        }

        if (arg[0] == '-') {
            fprintf(stderr, "Unknown undo option: %s\n", arg);
            return -1;
        }

        if (history_set) {
            fprintf(stderr, "Undo mode accepts at most one history file path.\n");
            return -1;
        }

        history_file = arg;
        history_set = 1;
    }

    *history_file_out = history_file;
    return 0;
}

int main(int argc, char **argv) {
    RenameOptions options;
    const char *positionals[4];
    size_t positional_count = 0;
    const char *history_file = DEFAULT_HISTORY_FILE;
    FileEntry *files = NULL;
    size_t file_count = 0;
    int assume_yes_for_undo = 0;
    int i;
    int rc;

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "undo") == 0) {
        if (parse_undo_args(argc, argv, &history_file, &assume_yes_for_undo) != 0) {
            print_usage(argv[0]);
            return 1;
        }

        return undo_last_batch(history_file, assume_yes_for_undo) == 0 ? 0 : 1;
    }

    memset(&options, 0, sizeof(options));
    options.match_mode = MATCH_MODE_EXTENSION;
    options.sort_mode = SORT_MODE_NAME;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--dryrun") == 0) {
            options.dry_run = 1;
            continue;
        }

        if (strcmp(arg, "--yes") == 0) {
            options.assume_yes = 1;
            continue;
        }

        if (strcmp(arg, "-r") == 0 || strcmp(arg, "--recursive") == 0) {
            options.recursive = 1;
            continue;
        }

        if (strcmp(arg, "--sort") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --sort.\n");
                return 1;
            }

            if (parse_sort_mode(argv[++i], &options.sort_mode) != 0) {
                fprintf(stderr, "Invalid sort mode. Use name, ctime, or size.\n");
                return 1;
            }
            continue;
        }

        if (strcmp(arg, "--regex") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing pattern for --regex.\n");
                return 1;
            }

            if (options.regex_pattern) {
                fprintf(stderr, "--regex can only be specified once.\n");
                return 1;
            }

            options.regex_pattern = argv[++i];
            options.match_mode = MATCH_MODE_REGEX;
            continue;
        }

        if (arg[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", arg);
            print_usage(argv[0]);
            return 1;
        }

        if (positional_count >= 4) {
            fprintf(stderr, "Too many positional arguments.\n");
            print_usage(argv[0]);
            return 1;
        }

        positionals[positional_count++] = arg;
    }

    if (options.match_mode == MATCH_MODE_REGEX) {
        if (positional_count != 3) {
            if (positional_count == 4) {
                fprintf(stderr,
                        "Extension matching and --regex matching are mutually exclusive. "
                        "Drop the extension positional argument when using --regex.\n");
            } else {
                fprintf(stderr,
                        "Regex mode requires: --regex <pattern> <folder> <prefix> <padding>.\n");
            }
            print_usage(argv[0]);
            return 1;
        }

        options.base_folder = positionals[0];
        options.prefix = positionals[1];
        if (parse_padding(positionals[2], &options.padding) != 0) {
            fprintf(stderr, "Padding must be a positive integer.\n");
            return 1;
        }
    } else {
        if (positional_count != 4) {
            fprintf(stderr,
                    "Extension mode requires: <folder> <ext> <prefix> <padding>.\n");
            print_usage(argv[0]);
            return 1;
        }

        options.base_folder = positionals[0];
        options.extension = positionals[1];
        options.prefix = positionals[2];

        if (options.extension[0] != '.' || options.extension[1] == '\0') {
            fprintf(stderr, "Extension must start with '.' and contain at least one character.\n");
            return 1;
        }

        if (parse_padding(positionals[3], &options.padding) != 0) {
            fprintf(stderr, "Padding must be a positive integer.\n");
            return 1;
        }
    }

    rc = collect_matching_files(&options, &files, &file_count);
    if (rc != 0) {
        return 1;
    }

    if (file_count == 0) {
        if (options.match_mode == MATCH_MODE_REGEX) {
            printf("No files matching regex '%s' found in %s.\n", options.regex_pattern,
                   options.base_folder);
        } else {
            printf("No files with extension %s found in %s.\n", options.extension,
                   options.base_folder);
        }
        return 0;
    }

    sort_files(files, file_count, options.sort_mode);

    rc = perform_renames(&options, files, file_count, DEFAULT_HISTORY_FILE);
    free_file_entries(files, file_count);

    if (rc != 0) {
        return 1;
    }

    return 0;
}
