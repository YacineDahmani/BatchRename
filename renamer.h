#ifndef RENAMER_H
#define RENAMER_H

#include <stddef.h>

typedef enum MatchMode {
    MATCH_MODE_EXTENSION = 0,
    MATCH_MODE_REGEX = 1
} MatchMode;

typedef enum SortMode {
    SORT_MODE_NAME = 0,
    SORT_MODE_CTIME = 1,
    SORT_MODE_SIZE = 2
} SortMode;

typedef struct RenameOptions {
    const char *base_folder;
    MatchMode match_mode;
    const char *extension;
    const char *regex_pattern;
    const char *prefix;
    int padding;
    int recursive;
    int dry_run;
    int assume_yes;
    SortMode sort_mode;
} RenameOptions;

typedef struct FileEntry {
    char *folder_path;
    char *name;
    long long creation_time;
    long long size_bytes;
} FileEntry;

int collect_matching_files(const RenameOptions *options, FileEntry **files_out, size_t *count_out);
void sort_files(FileEntry *files, size_t count, SortMode sort_mode);
int perform_renames(const RenameOptions *options, FileEntry *files, size_t count,
                    const char *history_path);
int undo_last_batch(const char *history_path, int assume_yes);
void free_file_entries(FileEntry *files, size_t count);

#endif
