#include "renamer.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define getcwd _getcwd
#endif

#define INITIAL_CAPACITY 16
#define NAME_BUFFER_SIZE 1024
#define PATH_BUFFER_SIZE 4096
#define HISTORY_LINE_BUFFER_SIZE 16384
#define REGEX_CLASS_SIZE 256
#define REGEX_REPEAT_UNBOUNDED (-1)

typedef enum PatternAtomType {
    PATTERN_LITERAL = 0,
    PATTERN_ANY = 1,
    PATTERN_CLASS = 2
} PatternAtomType;

typedef struct PatternToken {
    PatternAtomType type;
    unsigned char class_map[REGEX_CLASS_SIZE];
    char literal;
    int min_repeat;
    int max_repeat;
} PatternToken;

typedef struct CompiledPattern {
    PatternToken *tokens;
    size_t count;
} CompiledPattern;

typedef struct HistoryBatch {
    char **lines;
    size_t line_count;
    size_t start_line;
    size_t end_line;
    char **old_paths;
    char **new_paths;
    size_t count;
} HistoryBatch;

static SortMode g_sort_mode = SORT_MODE_NAME;

static char *duplicate_string(const char *source) {
    size_t len;
    char *copy;

    if (!source) {
        return NULL;
    }

    len = strlen(source);
    copy = (char *)malloc(len + 1);
    if (!copy) {
        return NULL;
    }

    memcpy(copy, source, len + 1);
    return copy;
}

static void free_string_array(char **values, size_t count) {
    size_t i;

    if (!values) {
        return;
    }

    for (i = 0; i < count; i++) {
        free(values[i]);
    }

    free(values);
}

static int build_path(char *out, size_t out_size, const char *folder, const char *name) {
    size_t folder_len;
    const char *separator = "";
    int written;

    if (!out || !folder || !name || out_size == 0) {
        return -1;
    }

    folder_len = strlen(folder);
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

static int line_starts_with(const char *line, const char *prefix) {
    size_t prefix_len;

    if (!line || !prefix) {
        return 0;
    }

    prefix_len = strlen(prefix);
    return strncmp(line, prefix, prefix_len) == 0;
}

static void trim_line_ending(char *line) {
    size_t len;

    if (!line) {
        return;
    }

    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
}

static int is_absolute_path(const char *path) {
    if (!path || path[0] == '\0') {
        return 0;
    }

#ifdef _WIN32
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':') {
        return 1;
    }

    if ((path[0] == '\\' && path[1] == '\\') || path[0] == '/' || path[0] == '\\') {
        return 1;
    }

    return 0;
#else
    return path[0] == '/';
#endif
}

static char *absolute_path_copy(const char *path) {
    char cwd[PATH_BUFFER_SIZE];
    char absolute_path[PATH_BUFFER_SIZE];

    if (!path) {
        return NULL;
    }

    if (is_absolute_path(path)) {
        return duplicate_string(path);
    }

    if (!getcwd(cwd, sizeof(cwd))) {
        perror("getcwd");
        return NULL;
    }

    if (build_path(absolute_path, sizeof(absolute_path), cwd, path) != 0) {
        fprintf(stderr, "Absolute path is too long: %s\n", path);
        return NULL;
    }

    return duplicate_string(absolute_path);
}

static int str_ends_with(const char *str, const char *suffix) {
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

static const char *extract_extension(const char *name) {
    const char *dot;

    if (!name) {
        return "";
    }

    dot = strrchr(name, '.');
    if (!dot || dot == name) {
        return "";
    }

    return dot;
}

static int find_path_index(char **paths, size_t count, const char *target_path) {
    size_t i;

    for (i = 0; i < count; i++) {
        if (paths[i] && strcmp(paths[i], target_path) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static int confirm_operation(const char *prompt, size_t count) {
    char answer[32];
    size_t i;

    printf("%s %zu renames? (y/n): ", prompt, count);
    if (!fgets(answer, sizeof(answer), stdin)) {
        return -1;
    }

    for (i = 0; answer[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)answer[i];
        if (isspace(ch)) {
            continue;
        }
        return tolower(ch) == 'y';
    }

    return 0;
}

static int append_pattern_token(PatternToken **tokens, size_t *count, size_t *capacity,
                                const PatternToken *token) {
    PatternToken *grown;

    if (*count == *capacity) {
        size_t new_capacity = (*capacity == 0) ? INITIAL_CAPACITY : (*capacity * 2);
        grown = (PatternToken *)realloc(*tokens, new_capacity * sizeof(PatternToken));
        if (!grown) {
            perror("realloc");
            return -1;
        }
        *tokens = grown;
        *capacity = new_capacity;
    }

    (*tokens)[*count] = *token;
    (*count)++;
    return 0;
}

static int parse_integer_value(const char *pattern, size_t *index, int *value_out) {
    long value = 0;
    int has_digit = 0;

    while (isdigit((unsigned char)pattern[*index])) {
        has_digit = 1;
        value = (value * 10) + (pattern[*index] - '0');
        if (value > INT_MAX) {
            return -1;
        }
        (*index)++;
    }

    if (!has_digit) {
        return -1;
    }

    *value_out = (int)value;
    return 0;
}

static int parse_char_class(const char *pattern, size_t *index, PatternToken *token,
                            char *error_buf, size_t error_buf_size) {
    size_t i = *index + 1;
    int negate = 0;
    int has_member = 0;
    int previous_char = -1;
    int previous_set = 0;

    memset(token->class_map, 0, sizeof(token->class_map));
    token->type = PATTERN_CLASS;

    if (pattern[i] == '^') {
        negate = 1;
        i++;
    }

    if (pattern[i] == '\0') {
        snprintf(error_buf, error_buf_size, "unterminated character class");
        return -1;
    }

    while (pattern[i] != '\0' && pattern[i] != ']') {
        int ch;
        int escaped = 0;

        if (pattern[i] == '\\' && pattern[i + 1] != '\0') {
            escaped = 1;
            i++;
        }

        ch = (unsigned char)pattern[i];

        if (!escaped && ch == '-' && previous_set && pattern[i + 1] != '\0' && pattern[i + 1] != ']') {
            int range_end;
            int range_start;
            int c;

            i++;
            if (pattern[i] == '\\' && pattern[i + 1] != '\0') {
                i++;
            }

            range_end = (unsigned char)pattern[i];
            range_start = previous_char;
            if (range_end < range_start) {
                int tmp = range_start;
                range_start = range_end;
                range_end = tmp;
            }

            for (c = range_start; c <= range_end; c++) {
                token->class_map[c] = 1;
            }

            has_member = 1;
            previous_char = range_end;
            previous_set = 1;
            i++;
            continue;
        }

        token->class_map[ch] = 1;
        has_member = 1;
        previous_char = ch;
        previous_set = 1;
        i++;
    }

    if (pattern[i] != ']') {
        snprintf(error_buf, error_buf_size, "unterminated character class");
        return -1;
    }

    if (!has_member) {
        snprintf(error_buf, error_buf_size, "empty character class");
        return -1;
    }

    if (negate) {
        int c;
        for (c = 0; c < REGEX_CLASS_SIZE; c++) {
            token->class_map[c] = (unsigned char)!token->class_map[c];
        }
    }

    *index = i + 1;
    return 0;
}

static int parse_quantifier(const char *pattern, size_t *index, PatternToken *token,
                            char *error_buf, size_t error_buf_size) {
    size_t i = *index;

    token->min_repeat = 1;
    token->max_repeat = 1;

    if (pattern[i] == '*') {
        token->min_repeat = 0;
        token->max_repeat = REGEX_REPEAT_UNBOUNDED;
        *index = i + 1;
        return 0;
    }

    if (pattern[i] == '+') {
        token->min_repeat = 1;
        token->max_repeat = REGEX_REPEAT_UNBOUNDED;
        *index = i + 1;
        return 0;
    }

    if (pattern[i] == '?') {
        token->min_repeat = 0;
        token->max_repeat = 1;
        *index = i + 1;
        return 0;
    }

    if (pattern[i] == '{') {
        int min_repeat;
        int max_repeat;

        i++;
        if (parse_integer_value(pattern, &i, &min_repeat) != 0) {
            snprintf(error_buf, error_buf_size, "invalid quantifier");
            return -1;
        }

        if (pattern[i] == '}') {
            max_repeat = min_repeat;
            i++;
        } else if (pattern[i] == ',') {
            i++;
            if (pattern[i] == '}') {
                max_repeat = REGEX_REPEAT_UNBOUNDED;
                i++;
            } else {
                if (parse_integer_value(pattern, &i, &max_repeat) != 0) {
                    snprintf(error_buf, error_buf_size, "invalid quantifier range");
                    return -1;
                }
                if (pattern[i] != '}') {
                    snprintf(error_buf, error_buf_size, "unterminated quantifier");
                    return -1;
                }
                i++;
            }
        } else {
            snprintf(error_buf, error_buf_size, "unterminated quantifier");
            return -1;
        }

        if (max_repeat != REGEX_REPEAT_UNBOUNDED && max_repeat < min_repeat) {
            snprintf(error_buf, error_buf_size, "invalid quantifier range");
            return -1;
        }

        token->min_repeat = min_repeat;
        token->max_repeat = max_repeat;
        *index = i;
        return 0;
    }

    return 0;
}

static int compile_pattern(const char *pattern, CompiledPattern *compiled, char *error_buf,
                           size_t error_buf_size) {
    PatternToken *tokens = NULL;
    size_t count = 0;
    size_t capacity = 0;
    size_t i = 0;

    if (!pattern || !compiled) {
        return -1;
    }

    while (pattern[i] != '\0') {
        PatternToken token;
        memset(&token, 0, sizeof(token));

        if (pattern[i] == '[') {
            if (parse_char_class(pattern, &i, &token, error_buf, error_buf_size) != 0) {
                free(tokens);
                return -1;
            }
        } else if (pattern[i] == '.') {
            token.type = PATTERN_ANY;
            i++;
        } else if (pattern[i] == '\\') {
            if (pattern[i + 1] == '\0') {
                snprintf(error_buf, error_buf_size, "trailing escape character");
                free(tokens);
                return -1;
            }
            token.type = PATTERN_LITERAL;
            token.literal = pattern[i + 1];
            i += 2;
        } else {
            token.type = PATTERN_LITERAL;
            token.literal = pattern[i];
            i++;
        }

        if (parse_quantifier(pattern, &i, &token, error_buf, error_buf_size) != 0) {
            free(tokens);
            return -1;
        }

        if (append_pattern_token(&tokens, &count, &capacity, &token) != 0) {
            snprintf(error_buf, error_buf_size, "out of memory while compiling regex");
            free(tokens);
            return -1;
        }
    }

    compiled->tokens = tokens;
    compiled->count = count;
    return 0;
}

static void free_compiled_pattern(CompiledPattern *compiled) {
    if (!compiled) {
        return;
    }

    free(compiled->tokens);
    compiled->tokens = NULL;
    compiled->count = 0;
}

static int token_matches_char(const PatternToken *token, unsigned char ch) {
    if (token->type == PATTERN_ANY) {
        return 1;
    }

    if (token->type == PATTERN_CLASS) {
        return token->class_map[ch] != 0;
    }

    return token->literal == (char)ch;
}

static int max_repeats_for_token(const PatternToken *token, const char *text, size_t text_len,
                                 size_t position) {
    int max_allowed;
    int repeats = 0;

    if (token->max_repeat == REGEX_REPEAT_UNBOUNDED) {
        max_allowed = (int)(text_len - position);
    } else {
        max_allowed = token->max_repeat;
    }

    while (repeats < max_allowed && position + (size_t)repeats < text_len) {
        if (!token_matches_char(token, (unsigned char)text[position + (size_t)repeats])) {
            break;
        }
        repeats++;
    }

    return repeats;
}

static int match_tokens_recursive(const PatternToken *tokens, size_t token_count, size_t token_index,
                                  const char *text, size_t text_len, size_t position,
                                  int require_full_match) {
    int repeats;
    const PatternToken *token;
    int max_repeats;

    if (token_index == token_count) {
        if (require_full_match) {
            return position == text_len;
        }
        return 1;
    }

    token = &tokens[token_index];
    max_repeats = max_repeats_for_token(token, text, text_len, position);
    if (max_repeats < token->min_repeat) {
        return 0;
    }

    for (repeats = max_repeats; repeats >= token->min_repeat; repeats--) {
        if (match_tokens_recursive(tokens, token_count, token_index + 1, text, text_len,
                                   position + (size_t)repeats, require_full_match)) {
            return 1;
        }

        if (repeats == 0) {
            break;
        }
    }

    return 0;
}

static int regex_match_compiled(const CompiledPattern *pattern, const char *text) {
    size_t start;
    size_t text_len;

    if (!pattern || !text) {
        return 0;
    }

    text_len = strlen(text);
    if (pattern->count == 0) {
        return 1;
    }

    for (start = 0; start <= text_len; start++) {
        if (match_tokens_recursive(pattern->tokens, pattern->count, 0, text, text_len, start,
                                   0)) {
            return 1;
        }
    }

    return 0;
}

static int append_file_entry(FileEntry **entries, size_t *count, size_t *capacity,
                             const char *folder_path, const char *name, long long creation_time,
                             long long size_bytes) {
    FileEntry *grown;
    FileEntry *entry;

    if (*count == *capacity) {
        size_t new_capacity = (*capacity == 0) ? INITIAL_CAPACITY : (*capacity * 2);
        grown = (FileEntry *)realloc(*entries, new_capacity * sizeof(FileEntry));
        if (!grown) {
            perror("realloc");
            return -1;
        }
        *entries = grown;
        *capacity = new_capacity;
    }

    entry = &(*entries)[*count];
    entry->folder_path = duplicate_string(folder_path);
    if (!entry->folder_path) {
        perror("malloc");
        return -1;
    }

    entry->name = duplicate_string(name);
    if (!entry->name) {
        perror("malloc");
        free(entry->folder_path);
        entry->folder_path = NULL;
        return -1;
    }

    entry->creation_time = creation_time;
    entry->size_bytes = size_bytes;
    (*count)++;
    return 0;
}

static int matches_filter(const RenameOptions *options, const CompiledPattern *compiled_pattern,
                          const char *name) {
    if (options->match_mode == MATCH_MODE_REGEX) {
        return regex_match_compiled(compiled_pattern, name);
    }

    return str_ends_with(name, options->extension);
}

static int scan_directory(const char *folder, const RenameOptions *options,
                          const CompiledPattern *compiled_pattern, FileEntry **entries,
                          size_t *count, size_t *capacity) {
    DIR *dir;
    struct dirent *entry;
    int status = 0;

    dir = opendir(folder);
    if (!dir) {
        fprintf(stderr, "Failed to open directory %s: %s\n", folder, strerror(errno));
        return -1;
    }

    while (1) {
        char full_path[PATH_BUFFER_SIZE];
        struct stat st;

        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            if (errno != 0) {
                fprintf(stderr, "Failed to read directory %s: %s\n", folder, strerror(errno));
                status = -1;
            }
            break;
        }

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (entry->d_name[0] == '.') {
            continue;
        }

        if (build_path(full_path, sizeof(full_path), folder, entry->d_name) != 0) {
            fprintf(stderr, "Path too long while scanning: %s\n", entry->d_name);
            status = -1;
            break;
        }

        if (stat(full_path, &st) != 0) {
            fprintf(stderr, "Failed to stat %s: %s\n", full_path, strerror(errno));
            status = -1;
            break;
        }

        if (S_ISDIR(st.st_mode)) {
            if (options->recursive) {
                if (scan_directory(full_path, options, compiled_pattern, entries, count, capacity) !=
                    0) {
                    status = -1;
                    break;
                }
            }
            continue;
        }

        if (!S_ISREG(st.st_mode)) {
            continue;
        }

        if (!matches_filter(options, compiled_pattern, entry->d_name)) {
            continue;
        }

        if (append_file_entry(entries, count, capacity, folder, entry->d_name, (long long)st.st_ctime,
                              (long long)st.st_size) != 0) {
            status = -1;
            break;
        }
    }

    if (closedir(dir) != 0) {
        fprintf(stderr, "Failed to close directory %s: %s\n", folder, strerror(errno));
        status = -1;
    }

    return status;
}

int collect_matching_files(const RenameOptions *options, FileEntry **files_out, size_t *count_out) {
    FileEntry *files = NULL;
    size_t count = 0;
    size_t capacity = 0;
    CompiledPattern compiled_pattern;
    int pattern_compiled = 0;
    int status = -1;

    memset(&compiled_pattern, 0, sizeof(compiled_pattern));

    if (!options || !options->base_folder || !files_out || !count_out) {
        return -1;
    }

    if (options->match_mode == MATCH_MODE_REGEX) {
        char error_buf[256];

        if (!options->regex_pattern || options->regex_pattern[0] == '\0') {
            fprintf(stderr, "Regex mode requires a non-empty pattern.\n");
            return -1;
        }

        if (compile_pattern(options->regex_pattern, &compiled_pattern, error_buf,
                            sizeof(error_buf)) != 0) {
            fprintf(stderr, "Invalid regex pattern: %s\n", error_buf);
            return -1;
        }

        pattern_compiled = 1;
    }

    if (scan_directory(options->base_folder, options,
                       pattern_compiled ? &compiled_pattern : NULL, &files, &count,
                       &capacity) != 0) {
        goto cleanup;
    }

    *files_out = files;
    *count_out = count;
    files = NULL;
    status = 0;

cleanup:
    if (pattern_compiled) {
        free_compiled_pattern(&compiled_pattern);
    }

    free_file_entries(files, count);
    return status;
}

static int compare_entries(const void *left, const void *right) {
    const FileEntry *lhs = (const FileEntry *)left;
    const FileEntry *rhs = (const FileEntry *)right;
    int cmp;

    if (g_sort_mode == SORT_MODE_NAME) {
        cmp = strcmp(lhs->name, rhs->name);
        if (cmp != 0) {
            return cmp;
        }
        return strcmp(lhs->folder_path, rhs->folder_path);
    }

    if (g_sort_mode == SORT_MODE_CTIME) {
        if (lhs->creation_time < rhs->creation_time) {
            return -1;
        }
        if (lhs->creation_time > rhs->creation_time) {
            return 1;
        }
    } else if (g_sort_mode == SORT_MODE_SIZE) {
        if (lhs->size_bytes < rhs->size_bytes) {
            return -1;
        }
        if (lhs->size_bytes > rhs->size_bytes) {
            return 1;
        }
    }

    cmp = strcmp(lhs->folder_path, rhs->folder_path);
    if (cmp != 0) {
        return cmp;
    }

    return strcmp(lhs->name, rhs->name);
}

void sort_files(FileEntry *files, size_t count, SortMode sort_mode) {
    if (!files || count < 2) {
        return;
    }

    g_sort_mode = sort_mode;
    qsort(files, count, sizeof(FileEntry), compare_entries);
}

static int append_history_batch(const char *history_path, char **old_paths, char **new_paths,
                                size_t count) {
    FILE *history;
    size_t i;

    history = fopen(history_path, "a");
    if (!history) {
        fprintf(stderr, "Failed to open history file %s: %s\n", history_path, strerror(errno));
        return -1;
    }

    if (fprintf(history, "BATCH %lld %zu\n", (long long)time(NULL), count) < 0) {
        fclose(history);
        return -1;
    }

    for (i = 0; i < count; i++) {
        char *abs_old = absolute_path_copy(old_paths[i]);
        char *abs_new = absolute_path_copy(new_paths[i]);

        if (!abs_old || !abs_new) {
            free(abs_old);
            free(abs_new);
            fclose(history);
            return -1;
        }

        if (fprintf(history, "%s\t%s\n", abs_old, abs_new) < 0) {
            free(abs_old);
            free(abs_new);
            fclose(history);
            return -1;
        }

        free(abs_old);
        free(abs_new);
    }

    if (fputs("END\n", history) == EOF) {
        fclose(history);
        return -1;
    }

    if (fclose(history) != 0) {
        return -1;
    }

    return 0;
}

static int append_line(char ***lines, size_t *count, size_t *capacity, const char *line) {
    char **grown;

    if (*count == *capacity) {
        size_t new_capacity = (*capacity == 0) ? INITIAL_CAPACITY : (*capacity * 2);
        grown = (char **)realloc(*lines, new_capacity * sizeof(char *));
        if (!grown) {
            perror("realloc");
            return -1;
        }
        *lines = grown;
        *capacity = new_capacity;
    }

    (*lines)[*count] = duplicate_string(line);
    if (!(*lines)[*count]) {
        perror("malloc");
        return -1;
    }

    (*count)++;
    return 0;
}

static int parse_batch_header_line(const char *line, size_t *count_out) {
    long long timestamp;

    if (sscanf(line, "BATCH %lld %zu", &timestamp, count_out) != 2) {
        return -1;
    }

    return 0;
}

static void free_history_batch(HistoryBatch *batch) {
    if (!batch) {
        return;
    }

    free_string_array(batch->lines, batch->line_count);
    free_string_array(batch->old_paths, batch->count);
    free_string_array(batch->new_paths, batch->count);
    memset(batch, 0, sizeof(*batch));
}

static int load_last_history_batch(const char *history_path, HistoryBatch *batch) {
    FILE *history;
    char buffer[HISTORY_LINE_BUFFER_SIZE];
    char **lines = NULL;
    size_t line_count = 0;
    size_t line_capacity = 0;
    size_t i;
    size_t last_start = 0;
    size_t last_end = 0;
    size_t last_count = 0;
    int found = 0;

    if (!history_path || !batch) {
        return -1;
    }

    history = fopen(history_path, "r");
    if (!history) {
        fprintf(stderr, "Failed to open history file %s: %s\n", history_path, strerror(errno));
        return -1;
    }

    while (fgets(buffer, sizeof(buffer), history)) {
        if (append_line(&lines, &line_count, &line_capacity, buffer) != 0) {
            fclose(history);
            free_string_array(lines, line_count);
            return -1;
        }
    }

    if (ferror(history)) {
        fclose(history);
        free_string_array(lines, line_count);
        fprintf(stderr, "Failed to read history file %s\n", history_path);
        return -1;
    }

    fclose(history);

    for (i = 0; i < line_count; i++) {
        size_t candidate_count;
        size_t candidate_end;
        size_t j;
        int valid = 1;

        if (!line_starts_with(lines[i], "BATCH ")) {
            continue;
        }

        if (parse_batch_header_line(lines[i], &candidate_count) != 0) {
            continue;
        }

        candidate_end = i + candidate_count + 1;
        if (candidate_end >= line_count) {
            continue;
        }

        for (j = 0; j < candidate_count; j++) {
            if (strchr(lines[i + 1 + j], '\t') == NULL) {
                valid = 0;
                break;
            }
        }

        if (!line_starts_with(lines[candidate_end], "END")) {
            valid = 0;
        }

        if (valid) {
            found = 1;
            last_start = i;
            last_end = candidate_end;
            last_count = candidate_count;
        }

        if (candidate_end > i) {
            i = candidate_end;
        }
    }

    if (!found) {
        free_string_array(lines, line_count);
        fprintf(stderr, "No valid batch entries found in history file %s\n", history_path);
        return -1;
    }

    batch->lines = lines;
    batch->line_count = line_count;
    batch->start_line = last_start;
    batch->end_line = last_end;
    batch->count = last_count;
    batch->old_paths = (char **)calloc(last_count, sizeof(char *));
    batch->new_paths = (char **)calloc(last_count, sizeof(char *));

    if (last_count > 0 && (!batch->old_paths || !batch->new_paths)) {
        perror("calloc");
        free_history_batch(batch);
        return -1;
    }

    for (i = 0; i < last_count; i++) {
        char *mapping = duplicate_string(lines[last_start + 1 + i]);
        char *separator;

        if (!mapping) {
            perror("malloc");
            free_history_batch(batch);
            return -1;
        }

        trim_line_ending(mapping);
        separator = strchr(mapping, '\t');
        if (!separator) {
            free(mapping);
            free_history_batch(batch);
            fprintf(stderr, "History batch format is invalid.\n");
            return -1;
        }

        *separator = '\0';
        separator++;

        batch->old_paths[i] = duplicate_string(mapping);
        batch->new_paths[i] = duplicate_string(separator);
        free(mapping);

        if (!batch->old_paths[i] || !batch->new_paths[i]) {
            perror("malloc");
            free_history_batch(batch);
            return -1;
        }
    }

    return 0;
}

static int rewrite_history_without_last_batch(const char *history_path, const HistoryBatch *batch) {
    FILE *history;
    size_t i;

    history = fopen(history_path, "w");
    if (!history) {
        fprintf(stderr, "Failed to rewrite history file %s: %s\n", history_path, strerror(errno));
        return -1;
    }

    for (i = 0; i < batch->line_count; i++) {
        if (i >= batch->start_line && i <= batch->end_line) {
            continue;
        }

        if (fputs(batch->lines[i], history) == EOF) {
            fclose(history);
            return -1;
        }
    }

    if (fclose(history) != 0) {
        return -1;
    }

    return 0;
}

int perform_renames(const RenameOptions *options, FileEntry *files, size_t count,
                    const char *history_path) {
    char **new_names = NULL;
    char **old_paths = NULL;
    char **new_paths = NULL;
    size_t i;
    size_t renamed_count = 0;
    int status = -1;

    if (!options || !files || !options->prefix) {
        return -1;
    }

    new_names = (char **)calloc(count, sizeof(char *));
    old_paths = (char **)calloc(count, sizeof(char *));
    new_paths = (char **)calloc(count, sizeof(char *));
    if (!new_names || !old_paths || !new_paths) {
        perror("calloc");
        goto cleanup;
    }

    for (i = 0; i < count; i++) {
        char generated_name[NAME_BUFFER_SIZE];
        const char *target_extension = options->extension;
        int written;
        char old_path[PATH_BUFFER_SIZE];
        char new_path[PATH_BUFFER_SIZE];

        if (options->match_mode == MATCH_MODE_REGEX) {
            target_extension = extract_extension(files[i].name);
        }

        written = snprintf(generated_name, sizeof(generated_name), "%s%0*d%s", options->prefix,
                           options->padding, (int)(i + 1), target_extension ? target_extension : "");
        if (written < 0 || (size_t)written >= sizeof(generated_name)) {
            fprintf(stderr, "Generated name is too long for item %zu.\n", i + 1);
            goto cleanup;
        }

        new_names[i] = duplicate_string(generated_name);
        if (!new_names[i]) {
            perror("malloc");
            goto cleanup;
        }

        if (build_path(old_path, sizeof(old_path), files[i].folder_path, files[i].name) != 0 ||
            build_path(new_path, sizeof(new_path), files[i].folder_path, new_names[i]) != 0) {
            fprintf(stderr, "Path too long while preparing rename for %s\n", files[i].name);
            goto cleanup;
        }

        old_paths[i] = duplicate_string(old_path);
        new_paths[i] = duplicate_string(new_path);
        if (!old_paths[i] || !new_paths[i]) {
            perror("malloc");
            goto cleanup;
        }
    }

    for (i = 0; i < count; i++) {
        size_t j;
        for (j = i + 1; j < count; j++) {
            if (strcmp(new_paths[i], new_paths[j]) == 0) {
                fprintf(stderr, "Rename conflict: generated duplicate target path %s\n", new_paths[i]);
                goto cleanup;
            }
        }
    }

    for (i = 0; i < count; i++) {
        struct stat st;

        if (strcmp(old_paths[i], new_paths[i]) == 0) {
            continue;
        }

        if (stat(new_paths[i], &st) == 0) {
            int source_index = find_path_index(old_paths, count, new_paths[i]);
            if (source_index >= 0) {
                fprintf(stderr,
                        "Rename conflict: target path %s is also a source path in this batch.\n",
                        new_paths[i]);
            } else {
                fprintf(stderr, "Rename conflict: target already exists: %s\n", new_paths[i]);
            }
            goto cleanup;
        }
    }

    if (options->dry_run) {
        printf("Dry run: %zu file(s) would be renamed.\n", count);
        for (i = 0; i < count; i++) {
            printf("%s -> %s\n", old_paths[i], new_paths[i]);
        }
        status = 0;
        goto cleanup;
    }

    if (!options->assume_yes) {
        int confirmed = confirm_operation("Proceed with", count);
        if (confirmed < 0) {
            fprintf(stderr, "Failed to read confirmation input.\n");
            goto cleanup;
        }

        if (!confirmed) {
            printf("Operation canceled.\n");
            status = 0;
            goto cleanup;
        }
    }

    for (i = 0; i < count; i++) {
        if (strcmp(old_paths[i], new_paths[i]) == 0) {
            printf("%s -> %s\n", old_paths[i], new_paths[i]);
            continue;
        }

        if (rename(old_paths[i], new_paths[i]) != 0) {
            fprintf(stderr, "Failed to rename %s to %s: %s\n", old_paths[i], new_paths[i],
                    strerror(errno));
            fprintf(stderr, "Operation stopped; some files may already be renamed.\n");
            goto cleanup;
        }

        printf("%s -> %s\n", old_paths[i], new_paths[i]);
        renamed_count++;
    }

    if (renamed_count > 0 && history_path && history_path[0] != '\0') {
        if (append_history_batch(history_path, old_paths, new_paths, count) != 0) {
            fprintf(stderr,
                    "Warning: renames succeeded but history could not be written to %s.\n",
                    history_path);
        }
    }

    status = 0;

cleanup:
    free_string_array(new_names, count);
    free_string_array(old_paths, count);
    free_string_array(new_paths, count);
    return status;
}

int undo_last_batch(const char *history_path, int assume_yes) {
    HistoryBatch batch;
    size_t i;
    int status = -1;

    memset(&batch, 0, sizeof(batch));

    if (load_last_history_batch(history_path, &batch) != 0) {
        return -1;
    }

    if (batch.count == 0) {
        printf("Last batch is empty; nothing to undo.\n");
        status = 0;
        goto cleanup;
    }

    if (!assume_yes) {
        int confirmed = confirm_operation("Proceed with undo of", batch.count);
        if (confirmed < 0) {
            fprintf(stderr, "Failed to read confirmation input.\n");
            goto cleanup;
        }

        if (!confirmed) {
            printf("Undo canceled.\n");
            status = 0;
            goto cleanup;
        }
    }

    for (i = 0; i < batch.count; i++) {
        struct stat st;

        if (stat(batch.new_paths[i], &st) != 0) {
            fprintf(stderr, "Undo failed: current path missing: %s\n", batch.new_paths[i]);
            goto cleanup;
        }

        if (stat(batch.old_paths[i], &st) == 0) {
            int source_index = find_path_index(batch.new_paths, batch.count, batch.old_paths[i]);
            if (source_index < 0) {
                fprintf(stderr, "Undo conflict: original path already exists: %s\n", batch.old_paths[i]);
                goto cleanup;
            }
        }
    }

    for (i = batch.count; i > 0; i--) {
        size_t idx = i - 1;

        if (strcmp(batch.new_paths[idx], batch.old_paths[idx]) == 0) {
            printf("%s -> %s\n", batch.new_paths[idx], batch.old_paths[idx]);
            continue;
        }

        if (rename(batch.new_paths[idx], batch.old_paths[idx]) != 0) {
            fprintf(stderr, "Failed to undo rename %s to %s: %s\n", batch.new_paths[idx],
                    batch.old_paths[idx], strerror(errno));
            fprintf(stderr, "Undo stopped; some files may already be restored.\n");
            goto cleanup;
        }

        printf("%s -> %s\n", batch.new_paths[idx], batch.old_paths[idx]);
    }

    if (rewrite_history_without_last_batch(history_path, &batch) != 0) {
        fprintf(stderr,
                "Warning: undo succeeded but failed to update history file %s.\n",
                history_path);
    }

    status = 0;

cleanup:
    free_history_batch(&batch);
    return status;
}

void free_file_entries(FileEntry *files, size_t count) {
    size_t i;

    if (!files) {
        return;
    }

    for (i = 0; i < count; i++) {
        free(files[i].folder_path);
        free(files[i].name);
    }

    free(files);
}
