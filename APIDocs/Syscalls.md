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
| 138 | `REPLACE` | Atomic file replacement from supplied bytes |
| 210 | `EXEPATH` | Copy the caller's own executable path to a buffer; returns its length or -1 (like `/proc/self/exe`). Lets a `.wiz` open its own bundle |
| 211 | `EXCHANGE` | Atomically swap two existing, closed regular files in the same directory; two path arguments, returns 0 or -1. Used to publish package upgrades without deleting the previous version first |
| 212 | `TRYWAIT` | Nonblocking exact-child wait: PID and optional exit-status pointer, returns child PID when collected, 0 while running, or -1 on error. Running/error results leave the status buffer untouched |
| 213 | `LAUNCH` | Path, origin (0=shell, 1=desktop), optional GUI-kind output pointer. Returns PID or -1. GUI apps are detached; desktop console apps get a private terminal host |
| 214 | `CONSOLE` | Console-host-only channel: action, buffer, size. Actions: 0=get target path, 1=drain output, 2=submit input, 3=EOF, 4=interrupt foreground program. Buffers limited to 4096 bytes |
| 200–205 | Window operations | See [Window API](WindowAPI.md) |

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
