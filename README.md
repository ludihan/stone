# stone — minimal stow-like symlink manager

Create and manage symlinks from a source directory into a target directory.

## Usage

```
stone [options] <source_dir> [target_dir]
```

For each regular file in `source_dir`, stone creates a symlink in
`target_dir` preserving the relative directory structure. Nested
directories are created as needed.

Symlinks inside `source_dir` are resolved and copied as regular files.

If a file already exists at the target path and differs from the
source, stone warns and aborts without creating any symlinks.
Use `-a` or `-o` to handle conflicts.

## Options

| Flag | Long form       | Description                                       |
|------|-----------------|---------------------------------------------------|
| `-h` | `--help`        | Show help                                         |
| `-d` | `--delete`      | Remove symlinks and copies created by a previous run |
| `-a` | `--adopt`       | Copy external file into source dir, then symlink  |
| `-o` | `--overwrite`   | Overwrite external file with source, then symlink |

## .stone-ignore

Place a `.stone-ignore` file in `source_dir` to skip files during
stow/delete operations. It follows `.gitignore` conventions:

| Feature    | Example       | Behavior                          |
|------------|---------------|-----------------------------------|
| Comment    | `# foo`       | Ignored line                      |
| Negation   | `!foo`        | Re-include a previously ignored file |
| Dir-only   | `foo/`        | Match directories only            |
| Anchored   | `/foo`        | Match only at root                |
| Basename   | `*.o`         | Match filename at any depth       |
| Full path  | `foo/bar`     | Match anchored to root            |
| `*`        | `*.txt`       | Any chars except `/`              |
| `**`       | `a/**/b`      | Any chars including `/`           |
| `?`        | `c?t`         | Single char except `/`            |

Last matching pattern wins (like `.gitignore`). The file itself is
never stowed.

## Built-in ignores

The following git-related entries are always ignored by default
(equivalent to an anchored entry in `.stone-ignore`):

- `.git/`
- `.github/`
- `.gitignore`
- `.gitattributes`
- `.gitmodules`

These can be overridden with a negation pattern in `.stone-ignore`
(e.g. `!.github/`).

## Build & Install

```
make
make install   # installs to ~/.local/bin
```

Default target is parent of `source_dir`. Specify a second argument
for a different target.
