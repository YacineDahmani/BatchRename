# BatchRename

BatchRename is a high-performance, precision command-line utility written in C for safe and efficient batch file renaming. It provides a robust set of features for organizing large collections of files with precision and safety.

## Features

- **Precision Matching**: Support for simple extension matching or advanced Regular Expressions.
- **Recursive Processing**: Global numbering across entire directory trees.
- **Safety by Design**: 
  - **Dry Runs**: Preview every change before it happens.
  - **Conflict Detection**: Checks for name collisions before execution.
  - **Two-Phase Atomic Renames**: Prevents data loss during chain or cycle renames.
  - **Rollback System**: Automatic attempt to revert on partial failures.
- **Instant Undo**: Revert the last successful batch with a single command.
- **Flexible Sorting**: Order files by name, creation time, or file size.
- **Smart Padding**: Customizable zero-padding for perfectly aligned filenames.

## Getting Started

### Prerequisites
- A C compiler (GCC, Clang, or MSVC)
- Make (optional)

### Build and Install
Clone the repository and build the binary:

```bash
git clone https://github.com/YacineDahmani/BatchRename.git
cd BatchRename
make
```

To run tests:
```bash
make test
```

## Usage

### Simple Extension Mode
Rename all `.jpg` files in a folder to `vacation_001.jpg`, `vacation_002.jpg`, etc.
```bash
./renamer ./photos .jpg vacation_ 3
```

### Advanced Regex Mode
Match specific patterns and keep original extensions.
```bash
./renamer --regex "IMG_[0-9]{4}" ./photos batch_ 2
```

### Undo Last Action
Accidentally renamed the wrong files? Run the undo command:
```bash
./renamer undo
```

## Options

| Flag | Description |
| :--- | :--- |
| `--dryrun` | View planned changes without modifying any files. |
| `--yes` | Skip interactive confirmation prompts. |
| `-r`, `--recursive` | Traverse subdirectories recursively. |
| `--sort <mode>` | Sort files by `name`, `ctime`, or `size`. (Default: `name`) |
| `--regex <pattern>` | Use regex matching (supports `^`, `$`, `*`, `+`, `?`, `{m,n}`, etc.). |

## Safety Architecture

BatchRename employs a two-phase execution model to ensure data integrity:
1. **Pre-flight Check**: Validates permissions and detects potential filename collisions.
2. **Phase 1 (Temp Move)**: Moves all target files to a unique temporary name.
3. **Phase 2 (Final Move)**: Renames temporary files to their final destination.

This architecture ensures that complex operations, such as "swapping" filenames or circular renames, are handled without data loss.

## Examples

**Sort by size and rename recursively**
```bash
./renamer -r --sort size ./assets .png asset_ 4
```

**Regex match at start of line with dry run**
```bash
./renamer --dryrun --regex "^temp_" ./work prefix_ 2
```

**Interactive undo with custom history file**
```bash
./renamer undo ./custom_history.log
```

## Windows Compatibility
BatchRename is fully compatible with Windows and utilizes WinAPI for robust path handling, including support for extended paths (exceeding 260 characters).

**PowerShell Usage:**
```powershell
.\renamer.exe --yes photos .jpg photo_ 3
```
