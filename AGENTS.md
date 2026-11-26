# kairocopy for coding agents

Use `kairocopy-cli` (the window program, `kairocopy.exe`, is for people). With `--json` every command prints one JSON
document on stdout; progress goes to stderr. Exit codes: 0 done, 1 failed or stopped, 2 wrong usage. Errors:
`{"ok": false, "error": {"code", "message"}}`.

kairocopy only copies: it never deletes or moves a source file. At the target it writes `<name>.kcpart` and renames it
when complete. A file already at the target is classed: the same (skipped), incomplete (copied again: smaller and the
start of the source, zeros where the end should be, or robocopy's 1980-01-01 mark), the same content with another time
(skipped; its time set to the source's when compared whole), a conflict (left alone unless `--conflict overwrite` or
`--conflict keep-both`, which writes `name（来自源）.ext`), older than the source (replaced), or only at the target (left
alone). Files of 64 MB or less are compared whole, larger ones in samples (`--strict`: whole).

| command | returns (`--json`) |
|---|---|
| `analyze <source>... --to <folder> [--important] [--strict]` | the plan: `all, to_copy, new, overwrite, incomplete, already_there, same_content, same_content_sampled, conflict, target_only, checked, not_important, online_only` (each `{files, bytes}`), `stale_parts`, `categories{photo, video, music, document, mail, chat, code, design, archive, other: {files, bytes, to_copy_files, to_copy_bytes}}`, `skipped_folders`, `free_bytes`, `target_is_hard_disk`, `unreadable_folders[]`, `conflict_samples[]`, `seconds` |
| `copy <source>... --to <folder> [--important] [--strict] [--conflict skip\|overwrite\|keep-both] [--verify] [--writers B,S]` | the plan, plus `ok, copied{files, bytes}, times_set, parts_removed, errors, in_use, stopped, disk_full, copy_seconds, mb_per_second, manifest, failed[{path, error}]` |
| `status <folder>` | `{folder, manifest, files, bytes, drive_files, drive_bytes, runs, last_run, unfinished?{run, sources, to, options, planned_files, copied_files}}` |

Each source (file or folder) lands in `<folder>` under its own name; a drive's root `C:\` lands as `<folder>\C`.
`--important` keeps the categories other than `other`. A file counts as already there when the target has it with the
same size and a modification time within 2 seconds.

Before a large copy: run `analyze` and tell the user what will be copied, how much is skipped, and whether it fits.
Running the same `copy` again resumes it. Before replacing conflicts, ask the user.
