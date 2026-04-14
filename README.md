# BatchRename

BatchRename is a C command-line tool for safe batch file renaming. It supports extension-based and regex-based matching, recursive processing, alternate sort modes, dry runs, interactive confirmation, and undo of the last successful batch.

## Build

Run:

make

Run basic regression checks:

make test

To remove build output:

make clean

## Usage

Extension mode:

./renamer [options] FOLDER EXT PREFIX PADDING

Regex mode:

./renamer [options] --regex PATTERN FOLDER PREFIX PADDING

Undo mode:

./renamer undo [--yes] [history-file]

## Options

- --dryrun: Show exactly what would be renamed without changing files. Dry runs do not write history.
- --yes: Skip interactive confirmation prompts.
- -r, --recursive: Traverse subfolders recursively. Numbering is global across the full matched tree.
- --sort MODE: Sort mode is name, ctime, or size.
- --regex PATTERN: Use regex matching instead of extension matching. Mutually exclusive with extension positional matching.

## Matching Rules

- Extension mode matches files whose names end with the provided extension.
- Regex mode searches within the full filename using substring-style regex search.
- Regex mode supports literals, `.`, character classes `[abc]` and `[^abc]`, escapes with `\\`, and quantifiers `*`, `+`, `?`, `{m}`, `{m,n}`, `{m,}`.
- Anchors are supported: `^` anchors to start of filename and `$` anchors to end.
- Invalid quantifier placement (for example starting with `*`) is rejected with an explicit parse error.
- Example pattern IMG_[0-9]{4} matches IMG_0001.jpg and IMG_1234.png.
- Hidden files and hidden directories (names starting with .) are skipped.

## Rename Output Rules

- New names are generated as PREFIX + zero-padded index + extension.
- In extension mode, the provided extension is used for output names.
- In regex mode, each file keeps its own original extension.

## Safety Features

- Dry run preview before touching files.
- Interactive confirmation by default for real renames with prompt: Proceed with N renames? (y/n)
- Preflight conflict detection before execution.
- Two-phase rename execution (`source -> temp -> target`) to handle chain/swap/cycle mappings safely.
- Automatic rollback attempt if an error happens during execution.
- Undo support for the last successful batch.

## Undo

- Every successful non-dry batch is appended to .rename_history.
- History entries store old and new absolute paths.
- Undo reverts only the latest batch and removes it from history after success.

Examples:

```text
./renamer --yes photos .jpg photo_ 3
./renamer --dryrun -r --sort size --regex IMG_[0-9]{4} photos batch_ 2
./renamer undo
./renamer undo --yes .rename_history
```

PowerShell equivalents on Windows:

```text
.\renamer --yes photos .jpg photo_ 3
.\renamer --dryrun -r --sort size --regex '^IMG_[0-9]{4}\.jpg$' photos batch_ 2
.\renamer undo --yes
```

## Notes

- On Windows, file operations use WinAPI path handling with extended path support for deep trees.
- Undo now uses the same two-phase safety model as forward renames.
