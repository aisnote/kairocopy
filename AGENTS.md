# kairocopy for coding agents

Use `kairocopy-cli` (the window program, `kairocopy.exe`, is for people). With `--json` every command prints one JSON
document on stdout; progress goes to stderr. Exit codes: 0 done, 1 failed or stopped, 2 wrong usage. Errors:
`{"ok": false, "error": {"code", "message"}}`.

kairocopy only copies: it never deletes or moves a source file. At the target it writes `<name>.kcpart` and renames it
when complete; an existing target file is replaced only when the source is newer.

| command | returns (`--json`) |
|---|---|
| `analyze <source>... --to <folder> [--important]` | the plan: `all, to_copy, new, overwrite, already_there, target_newer, not_important, online_only` (each `{files, bytes}`), `categories{photo, video, music, document, mail, chat, code, design, archive, other: {files, bytes, to_copy_files, to_copy_bytes}}`, `skipped_folders`, `free_bytes`, `target_is_hard_disk`, `unreadable_folders[]`, `target_newer_samples[]`, `seconds` |
| `copy <source>... --to <folder> [--important] [--verify] [--writers B,S]` | the plan, plus `ok, copied{files, bytes}, errors, in_use, stopped, disk_full, copy_seconds, mb_per_second, manifest, failed[{path, error}]` |
| `status <folder>` | `{folder, manifest, files, bytes, drive_files, drive_bytes, runs, last_run}`: what kairocopy copied there |

Each source (file or folder) lands in `<folder>` under its own name; a drive's root `C:\` lands as `<folder>\C`.
`--important` keeps the categories other than `other`. A file counts as already there when the target has it with the
same size and a modification time within 2 seconds.

Before a large copy: run `analyze` and tell the user what will be copied, how much is skipped, and whether it fits.
Running the same `copy` again resumes it.
