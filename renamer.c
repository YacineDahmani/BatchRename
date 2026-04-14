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
#include <wchar.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <windows.h>
#define getcwd _getcwd
#define get_process_id _getpid
#define PATH_SEPARATOR '\\'
#else
#include <unistd.h>
#define get_process_id getpid
#define PATH_SEPARATOR '/'
#endif

#define INITIAL_CAPACITY 16
#define NAME_BUFFER_SIZE 1024
#define HISTORY_LINE_BUFFER_SIZE 16384
#define REGEX_CLASS_SIZE 256
#define REGEX_REPEAT_UNBOUNDED (-1)
#define REGEX_MAX_PATTERN_LENGTH 512
#define REGEX_MAX_TOKEN_COUNT 256
#define TEMP_NAME_ATTEMPTS 2048

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
    int anchor_start;
    int anchor_end;
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

static int is_path_separator(char ch) {
    return ch == '/' || ch == '\\';
}

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

#ifdef _WIN32
static void normalize_windows_separators(char *path) {
    size_t i;

    if (!path) {
        return;
    }

    for (i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/') {
            path[i] = '\\';
        }
    }
}

static wchar_t *multi_to_wide_best_effort(const char *text) {
    int required;
    wchar_t *wide;

    if (!text) {
        return NULL;
    }

    required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (required <= 0) {
        required = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
        if (required <= 0) {
            return NULL;
        }

        wide = (wchar_t *)malloc((size_t)required * sizeof(wchar_t));
        if (!wide) {
            return NULL;
        }

        if (MultiByteToWideChar(CP_ACP, 0, text, -1, wide, required) <= 0) {
            free(wide);
            return NULL;
        }

        return wide;
    }

    wide = (wchar_t *)malloc((size_t)required * sizeof(wchar_t));
    if (!wide) {
        return NULL;
    }

    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, required) <= 0) {
        free(wide);
        return NULL;
    }

    return wide;
}

static char *wide_to_utf8_copy(const wchar_t *text) {
    int required;
    char *utf8;

    if (!text) {
        return NULL;
    }

    required = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (required <= 0) {
        return NULL;
    }

    utf8 = (char *)malloc((size_t)required);
    if (!utf8) {
        return NULL;
    }

    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, required, NULL, NULL) <= 0) {
        free(utf8);
        return NULL;
    }

    return utf8;
}

static wchar_t *get_full_path_wide_windows(const char *path) {
    DWORD required;
    wchar_t *input;
    wchar_t *full_path;

    if (!path) {
        return NULL;
    }

    input = multi_to_wide_best_effort(path);
    if (!input) {
        return NULL;
    }

    required = GetFullPathNameW(input, 0, NULL, NULL);
    if (required == 0) {
        free(input);
        return NULL;
    }

    full_path = (wchar_t *)malloc(((size_t)required + 1) * sizeof(wchar_t));
    if (!full_path) {
        free(input);
        return NULL;
    }

    if (GetFullPathNameW(input, required + 1, full_path, NULL) == 0) {
        free(full_path);
        free(input);
        return NULL;
    }

    free(input);
    return full_path;
}

static char *get_full_path_copy_windows(const char *path) {
    wchar_t *full_wide;
    char *utf8_path;

    full_wide = get_full_path_wide_windows(path);
    if (!full_wide) {
        return NULL;
    }

    utf8_path = wide_to_utf8_copy(full_wide);
    free(full_wide);
    if (!utf8_path) {
        return NULL;
    }

    normalize_windows_separators(utf8_path);
    return utf8_path;
}

static int has_extended_prefix_wide(const wchar_t *path) {
    return path && wcsncmp(path, L"\\\\?\\", 4) == 0;
}

static wchar_t *build_windows_extended_path_w(const char *path) {
    wchar_t *absolute;
    size_t len;

    if (!path) {
        return NULL;
    }

    absolute = get_full_path_wide_windows(path);
    if (!absolute) {
        return NULL;
    }

    if (has_extended_prefix_wide(absolute)) {
        return absolute;
    }

    len = wcslen(absolute);
    if (len < 240) {
        return absolute;
    }

    if (len >= 2 && absolute[0] == L'\\' && absolute[1] == L'\\') {
        const wchar_t *prefix = L"\\\\?\\UNC\\";
        size_t prefix_len = wcslen(prefix);
        wchar_t *with_prefix =
            (wchar_t *)malloc((prefix_len + (len - 2) + 1) * sizeof(wchar_t));
        if (!with_prefix) {
            free(absolute);
            return NULL;
        }

        wmemcpy(with_prefix, prefix, prefix_len);
        wmemcpy(with_prefix + prefix_len, absolute + 2, len - 1);
        with_prefix[prefix_len + (len - 2)] = L'\0';
        free(absolute);
        return with_prefix;
    }

    {
        const wchar_t *prefix = L"\\\\?\\";
        size_t prefix_len = wcslen(prefix);
        wchar_t *with_prefix = (wchar_t *)malloc((prefix_len + len + 1) * sizeof(wchar_t));
        if (!with_prefix) {
            free(absolute);
            return NULL;
        }

        wmemcpy(with_prefix, prefix, prefix_len);
        wmemcpy(with_prefix + prefix_len, absolute, len + 1);
        free(absolute);
        return with_prefix;
    }
}
#endif

static int path_equals(const char *left, const char *right) {
    size_t i = 0;

    if (!left || !right) {
        return 0;
    }

#ifdef _WIN32
    while (left[i] != '\0' && right[i] != '\0') {
        unsigned char lch = (unsigned char)left[i];
        unsigned char rch = (unsigned char)right[i];

        if (lch == '/') {
            lch = '\\';
        }
        if (rch == '/') {
            rch = '\\';
        }

        if (tolower(lch) != tolower(rch)) {
            return 0;
        }

        i++;
    }

    return left[i] == '\0' && right[i] == '\0';
#else
    return strcmp(left, right) == 0;
#endif
}

static char *build_path_alloc(const char *folder, const char *name) {
    size_t folder_len;
    size_t name_len;
    int needs_separator;
    char *joined;

    if (!folder || !name) {
        return NULL;
    }

    folder_len = strlen(folder);
    name_len = strlen(name);
    needs_separator = folder_len > 0 && !is_path_separator(folder[folder_len - 1]);

    joined = (char *)malloc(folder_len + (size_t)needs_separator + name_len + 1);
    if (!joined) {
        return NULL;
    }

    memcpy(joined, folder, folder_len);
    if (needs_separator) {
        joined[folder_len] = PATH_SEPARATOR;
    }
    memcpy(joined + folder_len + (size_t)needs_separator, name, name_len + 1);

    return joined;
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

static char *absolute_path_copy(const char *path) {
    if (!path) {
        return NULL;
    }

#ifdef _WIN32
    {
        char *full_path = get_full_path_copy_windows(path);
        if (!full_path) {
            fprintf(stderr, "Failed to resolve absolute path: %s\n", path);
        }
        return full_path;
    }
#else
    {
        char *cwd;
        char *joined;
        char *resolved;

        cwd = getcwd(NULL, 0);
        if (!cwd) {
            perror("getcwd");
            return NULL;
        }

        joined = build_path_alloc(cwd, path);
        free(cwd);
        if (!joined) {
            return NULL;
        }

        resolved = realpath(joined, NULL);
        if (!resolved) {
            resolved = joined;
        } else {
            free(joined);
        }

        return resolved;
    }
#endif
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
        if (paths[i] && path_equals(paths[i], target_path)) {
            return (int)i;
        }
    }

    return -1;
}

static int path_exists(const char *path) {
#ifdef _WIN32
    wchar_t *native_path = build_windows_extended_path_w(path);
    DWORD attrs;

    if (!native_path) {
        return 0;
    }

    attrs = GetFileAttributesW(native_path);
    free(native_path);
    return attrs != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

static int rename_path(const char *old_path, const char *new_path) {
#ifdef _WIN32
    wchar_t *native_old = build_windows_extended_path_w(old_path);
    wchar_t *native_new = build_windows_extended_path_w(new_path);
    int ok;

    if (!native_old || !native_new) {
        free(native_old);
        free(native_new);
        errno = ENOMEM;
        return -1;
    }

    ok = MoveFileExW(native_old, native_new, 0) != 0;
    free(native_old);
    free(native_new);

    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS) {
            errno = EEXIST;
        } else if (err == ERROR_ACCESS_DENIED) {
            errno = EACCES;
        } else {
            errno = EIO;
        }
        return -1;
    }

    return 0;
#else
    return rename(old_path, new_path);
#endif
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

static int is_escaped_character(const char *text, size_t index) {
    size_t backslashes = 0;

    while (index > 0 && text[index - 1] == '\\') {
        backslashes++;
        index--;
    }

    return (backslashes % 2) != 0;
}

static int compile_pattern(const char *pattern, CompiledPattern *compiled, char *error_buf,
                           size_t error_buf_size) {
    PatternToken *tokens = NULL;
    char *pattern_body = NULL;
    size_t body_start = 0;
    size_t body_end;
    size_t count = 0;
    size_t capacity = 0;
    size_t i = 0;
    size_t pattern_len;

    if (!pattern || !compiled) {
        return -1;
    }

    memset(compiled, 0, sizeof(*compiled));

    pattern_len = strlen(pattern);
    if (pattern_len > REGEX_MAX_PATTERN_LENGTH) {
        snprintf(error_buf, error_buf_size,
                 "pattern too long (max %d characters)", REGEX_MAX_PATTERN_LENGTH);
        return -1;
    }

    body_end = pattern_len;
    if (pattern_len > 0 && pattern[0] == '^') {
        compiled->anchor_start = 1;
        body_start = 1;
    }

    if (body_end > body_start && pattern[body_end - 1] == '$' &&
        !is_escaped_character(pattern, body_end - 1)) {
        compiled->anchor_end = 1;
        body_end--;
    }

    pattern_body = (char *)malloc((body_end - body_start) + 1);
    if (!pattern_body) {
        perror("malloc");
        return -1;
    }

    memcpy(pattern_body, pattern + body_start, body_end - body_start);
    pattern_body[body_end - body_start] = '\0';

    while (pattern_body[i] != '\0') {
        PatternToken token;
        memset(&token, 0, sizeof(token));

        if (pattern_body[i] == '*' || pattern_body[i] == '+' || pattern_body[i] == '?' ||
            pattern_body[i] == '{' || pattern_body[i] == '}') {
            snprintf(error_buf, error_buf_size, "quantifier without a target token");
            free(pattern_body);
            return -1;
        }

        if (pattern_body[i] == '[') {
            if (parse_char_class(pattern_body, &i, &token, error_buf, error_buf_size) != 0) {
                free(tokens);
                free(pattern_body);
                return -1;
            }
        } else if (pattern_body[i] == '.') {
            token.type = PATTERN_ANY;
            i++;
        } else if (pattern_body[i] == '\\') {
            if (pattern_body[i + 1] == '\0') {
                snprintf(error_buf, error_buf_size, "trailing escape character");
                free(tokens);
                free(pattern_body);
                return -1;
            }
            token.type = PATTERN_LITERAL;
            token.literal = pattern_body[i + 1];
            i += 2;
        } else {
            token.type = PATTERN_LITERAL;
            token.literal = pattern_body[i];
            i++;
        }

        if (parse_quantifier(pattern_body, &i, &token, error_buf, error_buf_size) != 0) {
            free(tokens);
            free(pattern_body);
            return -1;
        }

        if (count >= REGEX_MAX_TOKEN_COUNT) {
            snprintf(error_buf, error_buf_size,
                     "pattern has too many tokens (max %d)", REGEX_MAX_TOKEN_COUNT);
            free(tokens);
            free(pattern_body);
            return -1;
        }

        if (append_pattern_token(&tokens, &count, &capacity, &token) != 0) {
            snprintf(error_buf, error_buf_size, "out of memory while compiling regex");
            free(tokens);
            free(pattern_body);
            return -1;
        }
    }

    free(pattern_body);

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
    compiled->anchor_start = 0;
    compiled->anchor_end = 0;
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

static int match_tokens_iterative(const CompiledPattern *pattern, const char *text,
                                  size_t text_len, size_t start_index, int require_full_match) {
    size_t state_size = text_len + 1;
    unsigned char *current_states = NULL;
    unsigned char *next_states = NULL;
    size_t token_index;
    int matched = 0;

    current_states = (unsigned char *)calloc(state_size, sizeof(unsigned char));
    next_states = (unsigned char *)calloc(state_size, sizeof(unsigned char));
    if (!current_states || !next_states) {
        free(current_states);
        free(next_states);
        return 0;
    }

    current_states[start_index] = 1;

    for (token_index = 0; token_index < pattern->count; token_index++) {
        const PatternToken *token = &pattern->tokens[token_index];
        size_t position;
        int has_state = 0;

        memset(next_states, 0, state_size * sizeof(unsigned char));

        for (position = 0; position <= text_len; position++) {
            int repeats;
            int max_repeats;

            if (!current_states[position]) {
                continue;
            }

            max_repeats = max_repeats_for_token(token, text, text_len, position);
            if (max_repeats < token->min_repeat) {
                continue;
            }

            for (repeats = token->min_repeat; repeats <= max_repeats; repeats++) {
                next_states[position + (size_t)repeats] = 1;
                has_state = 1;
            }
        }

        if (!has_state) {
            free(current_states);
            free(next_states);
            return 0;
        }

        {
            unsigned char *tmp = current_states;
            current_states = next_states;
            next_states = tmp;
        }
    }

    if (require_full_match) {
        matched = current_states[text_len] != 0;
    } else {
        size_t position;
        for (position = 0; position <= text_len; position++) {
            if (current_states[position]) {
                matched = 1;
                break;
            }
        }
    }

    free(current_states);
    free(next_states);
    return matched;
}

static int regex_match_compiled(const CompiledPattern *pattern, const char *text) {
    size_t start_begin;
    size_t start_end;
    size_t start;
    size_t text_len;

    if (!pattern || !text) {
        return 0;
    }

    text_len = strlen(text);
    if (pattern->count == 0) {
        if (pattern->anchor_start && pattern->anchor_end) {
            return text_len == 0;
        }
        return 1;
    }

    start_begin = 0;
    start_end = text_len;
    if (pattern->anchor_start) {
        start_end = 0;
    }

    for (start = start_begin; start <= start_end; start++) {
        if (match_tokens_iterative(pattern, text, text_len, start, pattern->anchor_end)) {
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

#ifdef _WIN32
static long long filetime_to_longlong(const FILETIME *time_value) {
    ULARGE_INTEGER value;
    value.LowPart = time_value->dwLowDateTime;
    value.HighPart = time_value->dwHighDateTime;
    return (long long)value.QuadPart;
}

static int scan_directory(const char *folder, const RenameOptions *options,
                          const CompiledPattern *compiled_pattern, FileEntry **entries,
                          size_t *count, size_t *capacity) {
    char *search_pattern = NULL;
    wchar_t *native_search = NULL;
    HANDLE handle;
    WIN32_FIND_DATAW find_data;
    int status = 0;

    search_pattern = build_path_alloc(folder, "*");
    if (!search_pattern) {
        perror("malloc");
        return -1;
    }

    native_search = build_windows_extended_path_w(search_pattern);
    free(search_pattern);
    if (!native_search) {
        perror("malloc");
        return -1;
    }

    handle = FindFirstFileW(native_search, &find_data);
    free(native_search);
    if (handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Failed to open directory %s (Win32 error %lu)\n", folder,
                (unsigned long)GetLastError());
        return -1;
    }

    do {
        char *entry_name = wide_to_utf8_copy(find_data.cFileName);
        char *full_path;
        int is_directory;

        if (!entry_name) {
            fprintf(stderr, "Failed to decode a Windows path component in %s\n", folder);
            status = -1;
            break;
        }

        if (strcmp(entry_name, ".") == 0 || strcmp(entry_name, "..") == 0) {
            free(entry_name);
            continue;
        }

        if (entry_name[0] == '.') {
            free(entry_name);
            continue;
        }

        full_path = build_path_alloc(folder, entry_name);
        if (!full_path) {
            perror("malloc");
            free(entry_name);
            status = -1;
            break;
        }

        is_directory = (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (is_directory) {
            if (options->recursive &&
                (find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                if (scan_directory(full_path, options, compiled_pattern, entries, count,
                                   capacity) != 0) {
                    free(full_path);
                    free(entry_name);
                    status = -1;
                    break;
                }
            }

            free(full_path);
            free(entry_name);
            continue;
        }

        if (!matches_filter(options, compiled_pattern, entry_name)) {
            free(full_path);
            free(entry_name);
            continue;
        }

        if (append_file_entry(entries, count, capacity, folder, entry_name,
                              filetime_to_longlong(&find_data.ftCreationTime),
                              ((long long)find_data.nFileSizeHigh << 32) |
                                  (long long)find_data.nFileSizeLow) != 0) {
            free(full_path);
            free(entry_name);
            status = -1;
            break;
        }

        free(full_path);
        free(entry_name);
    } while (FindNextFileW(handle, &find_data));

    if (status == 0 && GetLastError() != ERROR_NO_MORE_FILES) {
        fprintf(stderr, "Failed while reading directory %s (Win32 error %lu)\n", folder,
                (unsigned long)GetLastError());
        status = -1;
    }

    FindClose(handle);
    return status;
}
#else
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

        {
            char *full_path = build_path_alloc(folder, entry->d_name);
            if (!full_path) {
                perror("malloc");
                status = -1;
                break;
            }

            if (stat(full_path, &st) != 0) {
                fprintf(stderr, "Failed to stat %s: %s\n", full_path, strerror(errno));
                free(full_path);
                status = -1;
                break;
            }

            if (S_ISDIR(st.st_mode)) {
                if (options->recursive) {
                    if (scan_directory(full_path, options, compiled_pattern, entries, count,
                                       capacity) != 0) {
                        free(full_path);
                        status = -1;
                        break;
                    }
                }
                free(full_path);
                continue;
            }

            if (!S_ISREG(st.st_mode)) {
                free(full_path);
                continue;
            }

            if (!matches_filter(options, compiled_pattern, entry->d_name)) {
                free(full_path);
                continue;
            }

            if (append_file_entry(entries, count, capacity, folder, entry->d_name,
                                  (long long)st.st_ctime, (long long)st.st_size) != 0) {
                free(full_path);
                status = -1;
                break;
            }

            free(full_path);
        }
    }

    if (closedir(dir) != 0) {
        fprintf(stderr, "Failed to close directory %s: %s\n", folder, strerror(errno));
        status = -1;
    }

    return status;
}
#endif

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

static char *path_parent_copy(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *last_separator = slash;
    size_t length;
    char *parent;

    if (!path) {
        return NULL;
    }

    if (!last_separator || (backslash && backslash > last_separator)) {
        last_separator = backslash;
    }

    if (!last_separator) {
        return duplicate_string(".");
    }

    length = (size_t)(last_separator - path);
    if (length == 0) {
        return duplicate_string((last_separator == backslash) ? "\\" : "/");
    }

    parent = (char *)malloc(length + 1);
    if (!parent) {
        return NULL;
    }

    memcpy(parent, path, length);
    parent[length] = '\0';
    return parent;
}

static int generate_unique_temp_path(const char *source_path, size_t sequence, char **old_paths,
                                     char **new_paths, char **temp_paths, size_t count,
                                     char **temp_path_out) {
    char *parent = NULL;
    unsigned long pid_value = (unsigned long)get_process_id();
    size_t attempt;

    if (!source_path || !temp_path_out) {
        return -1;
    }

    parent = path_parent_copy(source_path);
    if (!parent) {
        return -1;
    }

    for (attempt = 0; attempt < TEMP_NAME_ATTEMPTS; attempt++) {
        char temp_name[NAME_BUFFER_SIZE];
        char *candidate;
        int written = snprintf(temp_name, sizeof(temp_name),
                               ".renamer_tmp_%lld_%lu_%zu_%zu.tmp", (long long)time(NULL),
                               pid_value, sequence + 1, attempt + 1);

        if (written < 0 || (size_t)written >= sizeof(temp_name)) {
            continue;
        }

        candidate = build_path_alloc(parent, temp_name);
        if (!candidate) {
            free(parent);
            return -1;
        }

        if (!path_exists(candidate) && find_path_index(old_paths, count, candidate) < 0 &&
            find_path_index(new_paths, count, candidate) < 0 &&
            find_path_index(temp_paths, count, candidate) < 0) {
            *temp_path_out = candidate;
            free(parent);
            return 0;
        }

        free(candidate);
    }

    free(parent);
    return -1;
}

static int execute_two_phase_renames(char **old_paths, char **new_paths, size_t count,
                                     size_t *renamed_count_out) {
    char **temp_paths = NULL;
    int *active = NULL;
    int *state = NULL;
    size_t i;
    size_t renamed_count = 0;
    int status = -1;

    temp_paths = (char **)calloc(count, sizeof(char *));
    active = (int *)calloc(count, sizeof(int));
    state = (int *)calloc(count, sizeof(int));
    if (!temp_paths || !active || !state) {
        perror("calloc");
        goto cleanup;
    }

    for (i = 0; i < count; i++) {
        if (path_equals(old_paths[i], new_paths[i])) {
            continue;
        }

        active[i] = 1;
        if (!path_exists(old_paths[i])) {
            fprintf(stderr, "Rename conflict: source path is missing: %s\n", old_paths[i]);
            goto cleanup;
        }
    }

    for (i = 0; i < count; i++) {
        size_t j;
        for (j = i + 1; j < count; j++) {
            if (path_equals(new_paths[i], new_paths[j])) {
                fprintf(stderr, "Rename conflict: generated duplicate target path %s\n",
                        new_paths[i]);
                goto cleanup;
            }
        }
    }

    for (i = 0; i < count; i++) {
        int source_index;

        if (!active[i]) {
            continue;
        }

        if (!path_exists(new_paths[i])) {
            continue;
        }

        source_index = find_path_index(old_paths, count, new_paths[i]);
        if (source_index < 0 || !active[source_index]) {
            fprintf(stderr, "Rename conflict: target already exists: %s\n", new_paths[i]);
            goto cleanup;
        }
    }

    for (i = 0; i < count; i++) {
        if (!active[i]) {
            continue;
        }

        if (generate_unique_temp_path(old_paths[i], i, old_paths, new_paths, temp_paths, count,
                                      &temp_paths[i]) != 0) {
            fprintf(stderr, "Failed to allocate a temporary rename path for %s\n", old_paths[i]);
            goto cleanup;
        }
    }

    for (i = 0; i < count; i++) {
        if (!active[i]) {
            continue;
        }

        if (rename_path(old_paths[i], temp_paths[i]) != 0) {
            fprintf(stderr, "Failed to move %s to temporary path %s: %s\n", old_paths[i],
                    temp_paths[i], strerror(errno));
            goto rollback;
        }

        state[i] = 1;
    }

    for (i = 0; i < count; i++) {
        if (!active[i]) {
            continue;
        }

        if (rename_path(temp_paths[i], new_paths[i]) != 0) {
            fprintf(stderr, "Failed to rename %s to %s: %s\n", old_paths[i], new_paths[i],
                    strerror(errno));
            goto rollback;
        }

        state[i] = 2;
        renamed_count++;
    }

    status = 0;
    goto cleanup;

rollback:
    for (i = count; i > 0; i--) {
        size_t idx = i - 1;

        if (state[idx] != 2) {
            continue;
        }

        if (rename_path(new_paths[idx], temp_paths[idx]) != 0) {
            fprintf(stderr, "Rollback warning: failed to move %s back to %s\n", new_paths[idx],
                    temp_paths[idx]);
        } else {
            state[idx] = 1;
        }
    }

    for (i = count; i > 0; i--) {
        size_t idx = i - 1;

        if (state[idx] != 1) {
            continue;
        }

        if (rename_path(temp_paths[idx], old_paths[idx]) != 0) {
            fprintf(stderr, "Rollback warning: failed to restore %s\n", old_paths[idx]);
        } else {
            state[idx] = 0;
        }
    }

cleanup:
    if (renamed_count_out) {
        *renamed_count_out = renamed_count;
    }

    free_string_array(temp_paths, count);
    free(active);
    free(state);
    return status;
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

        old_paths[i] = build_path_alloc(files[i].folder_path, files[i].name);
        new_paths[i] = build_path_alloc(files[i].folder_path, new_names[i]);
        if (!old_paths[i] || !new_paths[i]) {
            perror("malloc");
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

    if (execute_two_phase_renames(old_paths, new_paths, count, &renamed_count) != 0) {
        fprintf(stderr, "Operation failed; rollback was attempted for completed steps.\n");
        goto cleanup;
    }

    for (i = 0; i < count; i++) {
        printf("%s -> %s\n", old_paths[i], new_paths[i]);
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
    size_t renamed_count = 0;
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

    if (execute_two_phase_renames(batch.new_paths, batch.old_paths, batch.count,
                                  &renamed_count) != 0) {
        fprintf(stderr, "Undo failed; rollback was attempted for completed steps.\n");
        goto cleanup;
    }

    for (i = 0; i < batch.count; i++) {
        printf("%s -> %s\n", batch.new_paths[i], batch.old_paths[i]);
    }

    if (renamed_count > 0 && rewrite_history_without_last_batch(history_path, &batch) != 0) {
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
