#ifndef RENAMER_H
#define RENAMER_H

#include <stddef.h>

int str_ends_with(const char *str, const char *suffix);
int collect_matching_files(const char *folder, const char *ext, char ***files_out, size_t *count_out);
void sort_files(char **files, size_t count);
int perform_renames(const char *folder, const char *ext, const char *prefix, int padding,
                    char **files, size_t count);
void free_file_list(char **files, size_t count);

#endif
