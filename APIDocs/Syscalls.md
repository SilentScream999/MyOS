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
| 216 | `OPEN_WITH` | App path, document path, origin (0=desktop/default, 1=shell). Both paths are resolved and the child receives a private document path via DOCUMENT_PATH. Desktop console handlers remain refused; shell origin permits foreground console children and wait/Ctrl+C handling. Returns PID or -1 |
| 217 | `TEXT_CLIPBOARD` | ABI 7. Action, buffer, size. Action 0 sets up to 65536 text bytes without NUL and returns 0. Action 1 copies current text including the terminating NUL and returns its byte length. Errors return -1 and preserve the clipboard and output. Win+V shows up to 12 distinct nonempty entries. Pinned entries stay in history until deleted, unpinned and evicted, or restarted. Setting empty text clears the current value |
| 218 | `PROCESS_INFO` | ABI 8. `ProcessInfo*`, capacity (0–256), `ProcessTotals*`, optional `ProcessMetadata*` (ABI 10); returns total process count or -1 for invalid arguments. Copies at most capacity records and totals with IRQs disabled. Structures are in `src/core/process_info.h`; state is 0=ready, 1=running, 2=blocked, 3=dead. CPU counters use cumulative TSC cycles, exclude halts, and include syscall/interrupt work on the current task. Compute usage from differences between snapshots; a larger return count signals truncation. Metadata adds snapshot milliseconds and per-record categories: 0=program (GUI/private console/foreground terminal app), 1=background, 2=kernel task. Existing three-argument callers retain their original layouts and behavior |
| 219 | `KERNEL_CPU` | ABI 9. `KernelCpuInfo*`; returns 0 or -1. Opts into PID 0 CPU profiling for two seconds; each read renews the lease. Reports cumulative exclusive category cycles, scope-entry counts, snapshot TSC and timer ticks. Nested interrupt/scheduler scopes replace the surrounding input category, and blocked/halted time is excluded. Counters reset after an expired lease; discard the first sample when enabling. Profiling adds overhead; it stops automatically when reads cease |
| 220 | `EJECT` | Path to an external FAT32/exFAT volume or an item inside it. Flushes and detaches all mounted volumes on the same physical drive. Returns 0 on success, 1 on logical eject without explicit device cache-flush support, -1 for an unsupported/unavailable path, -2 when files are open, or -3 when flushing fails. Old handles retain the retired volume |
| 221 | `STORAGE_STATE` | Action 0 returns the storage namespace generation. Action 1 takes a path as the second argument and returns 1 if it supports external-drive eject, otherwise 0. Invalid requests return -1. Explorer polls the generation to refresh drives without directory polling |
| 222 | `WIN_PUBLISH` | Owner-only immutable surface snapshot; optional source-space top damage height, zero for full image. Returns 0 on publication or -1 on invalid arguments/deferred publication. See the window API |
| 223 | `WIN_ALPHA` | ABI 11. Owned window ID and mode: 0=opaque (default), 1=premultiplied source-over. Returns 0 or -1; changing modes damages the full window. See the window API |
| 224 | `AUDIO` | `(action, arg, bytes)`. Actions 0–5 retain HDA diagnostics/firmware mute controls; actions 6–17 add master/stream volume and an exclusive PCM stream, described below. Errors: -1 failed/unavailable hardware, stream underflow or DMA failure; -2 busy/wrong stream owner; -3 unsupported route; -4 invalid arguments/user buffer; -5 no alternate DAC; -6 unavailable firmware backend. Operations may sleep and check interruption. See [audio](../docs/AUDIO.md) |
| 225 | `FIRMWARE_DIAG` | `(action,arg,buffer,capacity,offset)`. Action 0 arms an owner-scoped Fn trace for arg=1000..45000 ms. Action 1 copies trace bytes starting at arg; zero buffer/capacity returns total trace bytes. Action 2 exports table selector arg: 0=FADT, 1=ECDT, 2=DSDT, 3..34=SSDT occurrences; zero buffer/capacity queries size, otherwise copies from offset. Action 3 marks stage arg=0..15; action 4 disarms. Trace read/mark/stop require the owning PID. Tables are read-only, checksummed, limited to 1 MiB and backed by ACPI memory. Output buffers require user-writable pages and 1..65536-byte capacity. Errors: -1 missing/invalid table or task, -2 capture busy/wrong owner, -4 invalid arguments/buffer |
| 200–208 | Window operations, including native resizing (206–208, ABI 4) | See [Window API](WindowAPI.md) |
| 209 | `WIN_ROLE` | Owned window ID, role (0=app, 1=common file dialog); returns 0 or -1. ABI 6; gives common dialogs the Explorer icon without changing ownership |

`AUDIO` actions:

| Action | Arguments and result |
| --- | --- |
| 0 | arg=0; probe analog routes, return mask speaker=2/headphone=4/line=8 |
| 1 / 2 | arg=kind (0=automatic, 1=speaker, 2=headphone, 3=line); diagnostic tone / alternate-DAC tone; return 0 |
| 3 / 4 / 5 | arg=0; read E470 EC audio byte / clear hardware mute bit / toggle hardware mute bit; read returns raw byte, writes return 0 after readback |
| 6 | Return volume percentage in bits 0–7 and software mute in bit 8; no hardware probe |
| 7 | arg=kind; open exclusive stereo interleaved signed 16-bit little-endian PCM. Third argument 0 preserves the original 48000 Hz interface and returns 0. Optional requested rate 44100/48000 negotiates native support and returns the actual rate (44100 if requested and advertised, otherwise 48000). Auto prefers detected plugged headphones. Supported E470 firmware mute is cleared at open; shared software mute is preserved |
| 8 | arg=user PCM pointer, bytes=4..65536 and a multiple of 4; verify all pages present/user-readable, copy to bounded DMA ring with backpressure, return bytes accepted or a negative error. A failure can follow a partial transfer: stop/close instead of retrying the whole buffer |
| 9 | Drain all submitted PCM; starts short streams below the prefill threshold; return 0 after playback finishes |
| 10 | Stop and close the owning stream, return 0. Owner exit/interruption also triggers cleanup |
| 11 | arg=volume 0..100; update shared volume, return 0; does not change software mute |
| 12 | arg=0 unmute software / 1 mute software / 2 toggle software mute; return 0; volume level is retained |
| 13 | ABI 12. Same buffer rules as action 8; copy available space and return bytes accepted (possibly 0 or partial) without waiting for DMA space. Advance the source pointer before retrying; negative errors require closing the stream |
| 14 | ABI 12. arg=1 pause / 0 resume. Preserve queued PCM and DMA position; return 0. Paused streams retain ownership and do not need periodic DMA service |
| 15 | ABI 12. Return queued PCM bytes. arg=1 also starts a short stream below the prefill threshold; arg=0 reads the service's latest counters without waking it. Poll for zero to finish without a blocking drain |
| 16 | ABI 12. arg=0; read the service's latest consumed PCM bytes, clamped to bytes submitted, without waking it. Divide by actual rate times 4 for elapsed seconds |
| 17 | ABI 13. Owner-only stream volume: arg=0..100 and bytes=0 unmuted / 1 muted. Publish app gain/mute without changing shared system volume/mute. The audio worker applies the combined app/system level; a new stream starts at 100%, unmuted |

Only the opening PID can write/drain/close or use actions 13–17. Actions 11/12 control the global mixer; action 17 controls only the owning stream.
There is one 256 KiB DMA ring with a reserved page and a 128 KiB start threshold.
An exhausted stream is stopped; writing again after underrun fails, so close
and reopen. PCM pages/descriptors stay pinned even on failed device shutdown.
The MP3 decoder belongs to userspace and is not part of this syscall ABI.

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
