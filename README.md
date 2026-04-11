# BatchRename

BatchRename is a small C command-line tool that renames every file in a folder that matches a given extension. It processes files in sorted order, then assigns new names using a custom prefix and zero-padded numbering.

## What it does

Example:

```bash
./renamer ./photos .jpg photo_ 3
```

If the folder contains files like `DSC001.jpg`, `IMG_4892.jpg`, and `a_pic.jpg`, the tool will rename them in alphabetical order to:

```text
photo_001.jpg
photo_002.jpg
photo_003.jpg
```

Hidden files and files that do not end with the requested extension are ignored.

## How it works

The program is split into two main parts:

1. `main.c` handles argument parsing and validation.
2. `renamer.c` scans the directory, sorts matching filenames, builds the new names, and performs the renames.

The workflow is:

1. Read the folder path, extension, prefix, and padding from the command line.
2. Open the folder with `opendir()` and collect matching filenames with `readdir()`.
3. Sort the collected filenames with `qsort()` so the rename order is predictable.
4. Build each new name with `snprintf()` using zero-padded numbering.
5. Rename each file with `rename()` and print the mapping.
6. Free all allocated memory before exiting.

## Usage

```bash
./renamer <folder> <ext> <prefix> <padding>
```

Arguments:

- `folder`: folder containing the files to rename
- `ext`: file extension to match, for example `.jpg`
- `prefix`: new filename prefix, for example `photo_`
- `padding`: number of digits for numbering, for example `3` produces `001`, `002`, `003`

## Build

Build the project with `make`:

```bash
make
```

To remove build output:

```bash
make clean
```

## Example

```bash
./renamer ./photos .jpg photo_ 3
```

With input files:

```text
DSC001.jpg
IMG_4892.jpg
a_pic.jpg
```

The output becomes:

```text
photo_001.jpg
photo_002.jpg
photo_003.jpg
```

## Notes

- The tool currently works on one folder level only.
- Files are renamed in alphabetical order, not by creation time.
- The program checks for basic input errors and rename conflicts.
- Generated files and temporary test folders are ignored by Git.
