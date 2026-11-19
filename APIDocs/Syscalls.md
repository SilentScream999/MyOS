# Native syscall reference

Updated 2026-11-11. Source of truth: `src/core/syscall.h`, `src/fs/fileinfo.h`
and `user/init.c`. These numbers define WyzarOS's ABI, not Linux compatibility.

Use `syscall`: number in `rax`, up to five dispatched arguments in `rdi`, `rsi`,
`rdx`, `r10`, `r8`, return value in `rax`. A sixth argument is not dispatched;
window-create flags are currently fixed to zero. Inline wrappers must account
for `rcx`/`r11` and memory effects. User-pointer validation remains incomplete.

| Number | Name | Purpose |
|---|---|---|
| 0 / 1 / 2 / 3 | `READ` / `WRITE` / `OPEN` / `CLOSE` | TTY on fd 0/1; VFS files on allocated descriptors |
| 4 / 8 | `STAT` / `LSEEK` | Metadata; file offset using SET/CUR/END |
| 16 / 17 | `SCROLL` / `TTYRAW` | Terminal scrollback and raw input |
| 18 / 19 | `TTYECHO` / `TTYFLUSH_INPUT` | Set echo (returns previous state); discard unread input |
| 20 | `TTYTAKE_LAUNCH` | Registered shell consumes a queued Start-menu path |
| 35 / 39 / 40 | `SLEEP` / `GETPID` / `READDIR` | Timed blocking, task ID, indexed directory enumeration |
| 59 / 60 / 61 | `SPAWN` / `EXIT` / `WAITPID` | Native ELF launch, retained status, wait for one exact child |
| 74 / 77 | `FSYNC` / `FTRUNCATE` | Synchronize or change file length |
| 79 / 80 | `GETCWD` / `CHDIR` | Current working directory |
| 82 / 83 / 84 | `RENAME` / `MKDIR` / `RMDIR` | Rename without overwrite; create/remove empty directory |
| 87 | `UNLINK` | Remove a closed file |
| 88 / 169 | `SHUTDOWN` / `REBOOT` | Attempt filesystem close, then power action |
| 137 | `STATFS` | Filesystem capacity/free space |
| 138 | `REPLACE` | File replacement from supplied bytes. ext4 uses atomic replacement. External FAT32/exFAT stages the new content, retains a backup during publication, and recovers interrupted replacements on lookup |
| 210 | `EXEPATH` | Copy the caller's own executable path to a buffer; returns its length or -1 (like `/proc/self/exe`). Lets a `.wiz` open its own bundle |
| 211 | `EXCHANGE` | Atomically swap two existing, closed regular files in the same directory; two path arguments, returns 0 or -1. Used to publish package upgrades without deleting the previous version first |
| 212 | `TRYWAIT` | Nonblocking exact-child wait: PID and optional exit-status pointer, returns child PID when collected, 0 while running, or -1 on error. Running/error results leave the status buffer untouched |
| 213 | `LAUNCH` | Path, origin (0=shell, 1=desktop), optional GUI-kind output pointer. Returns PID or -1. GUI apps are detached; desktop console apps get a private terminal host |
| 214 | `CONSOLE` | Console-host-only channel. Arguments are action, buffer, size. Actions are 0=get target path, 1=drain output, 2=submit input, 3=EOF, 4=interrupt foreground program, 5=get root child PID (0 before spawn), 6=get initial geometry (four int32 values for x, y, width, height), 7=wait on an owned window ID supplied as buffer. Wait wakes for window events, console output, child exit or a 500 ms cursor deadline. It returns `(tick_ms << 1) \| cursor_active`. Byte buffers are limited to 4096 bytes. The kernel checks host ownership and foreground channel membership |
| 215 | `DOCUMENT_PATH` | Buffer, capacity; copy the caller's launch document path including NUL. Returns byte length, zero for a normal launch, or -1 for an invalid/short buffer |
| 216 | `OPEN_WITH` | App path, document path; launch a detached GUI package with a private copy of the resolved document path. Returns PID or -1; console handlers are refused |
| 217 | `TEXT_CLIPBOARD` | ABI 7. Action, buffer, size. Action 0 sets up to 65536 text bytes without NUL and returns 0. Action 1 copies current text including the terminating NUL and returns its byte length. Errors return -1 and preserve the clipboard and output. Win+V shows up to 12 distinct nonempty entries. Pinned entries stay in history until deleted, unpinned and evicted, or restarted. Setting empty text clears the current value |
| 218 | `PROCESS_INFO` | ABI 8. `ProcessInfo*`, capacity (0–256), `ProcessTotals*`, optional `ProcessMetadata*` (ABI 10); returns total process count or -1 for invalid arguments. Copies at most capacity records and totals with IRQs disabled. Structures are in `src/core/process_info.h`; state is 0=ready, 1=running, 2=blocked, 3=dead. CPU counters use cumulative TSC cycles, exclude halts, and include syscall/interrupt work on the current task. Compute usage from differences between snapshots; a larger return count signals truncation. Metadata adds snapshot milliseconds and per-record categories: 0=program (GUI/private console/foreground terminal app), 1=background, 2=kernel task. Existing three-argument callers retain their original layouts and behavior |
| 219 | `KERNEL_CPU` | ABI 9. `KernelCpuInfo*`; returns 0 or -1. Opts into PID 0 CPU profiling for two seconds; each read renews the lease. Reports cumulative exclusive category cycles, scope-entry counts, snapshot TSC and timer ticks. Nested interrupt/scheduler scopes replace the surrounding input category, and blocked/halted time is excluded. Counters reset after an expired lease; discard the first sample when enabling. Profiling adds overhead; it stops automatically when reads cease |
| 220 | `EJECT` | Path to an external FAT32/exFAT volume or an item inside it. Flushes and detaches all mounted volumes on the same physical drive. Returns 0 on success, 1 on logical eject without explicit device cache-flush support, -1 for an unsupported/unavailable path, -2 when files are open, or -3 when flushing fails. Old handles retain the retired volume |
| 221 | `STORAGE_STATE` | Action 0 returns the storage namespace generation. Action 1 takes a path as the second argument and returns 1 if it supports external-drive eject, otherwise 0. Invalid requests return -1. Explorer polls the generation to refresh drives without directory polling |
| 200–208 | Window operations, including native resizing (206–208, ABI 4) | See [Window API](WindowAPI.md) |
| 209 | `WIN_ROLE` | Owned window ID, role (0=app, 1=common file dialog); returns 0 or -1. ABI 6; gives common dialogs the Explorer icon without changing ownership |

Paths accept absolute and CWD-relative forms, `.` and `..`, with a current
255-byte userspace limit. File metadata structures are in `src/fs/fileinfo.h`;
timestamp fields can be unknown while UTC semantics are unfinished.

The shell uses `REPLACE` for `write`, ordinary extension for `append`, and
non-overwriting `RENAME` for `mv`. Direct `O_TRUNC` writes have different,
non-atomic behavior. See [storage](../docs/PERSISTENT_STORAGE.md).

`WAITPID` supports one exact child, not process groups or a full POSIX wait API.
Start-menu requests interrupt shell stdin with `-2`; the shell then consumes the
path through syscall 20 and uses the same foreground launch/wait path as `run`.
Ctrl+C cancellation and descriptor/window cleanup are described in
[applications](../docs/APP_LOADING.md).
