# Project handoff

Updated 2026-11-24. Start with the current status below.
Start with [README](../README.md), then the guide for the area being changed.
Pending work is in [NEXT_STEPS](NEXT_STEPS.md); commands are in [TESTING](TESTING.md).

## Current system

WyzarOS is a single-core x86-64 OS with a C++ kernel, C/C++ userspace, Limine boot,
preemptive tasks, static ELF loading and a framebuffer desktop. The long-term
target is a GPU-composited desktop with installable applications.

The retained Rainbow checkpoint reached 58–60 FPS in steady tall
half-screen translucency samples and 58–59 FPS with the grouped app picker
open on Kaby Lake. Brief drops remain during layout changes. The standard
build enables this path after Intel hardware checks. `GPU_RCS_ALPHA=0` selects
CPU SIMD translucency, and `GPU=0` selects CPU-only rendering.
USB discovery now lets the timer and compositor run during synchronous
probes. See [build configurations](../BUILD_CONFIGS.txt) and the
[GPU roadmap](GPU_DESKTOP_ROADMAP.md) for the current paths.

| Area | Implemented | Details |
|---|---|---|
| Boot | ISO/UEFI USB; framebuffer mapped before drawing; RAM recovery | [Storage](PERSISTENT_STORAGE.md) |
| Processes | Private kernel stacks/address spaces, spawn, exact-child wait, foreground cancellation | [Apps](APP_LOADING.md) |
| Desktop | Surface-backed windows, damage tracking, taskbar, Start, CPU presentation | [Display roadmap](GPU_DESKTOP_ROADMAP.md) |
| App resources | Portable `.wiz` packages, verified install/pin/remove, metadata/icons, `/apps` and `/bin` discovery | [Packages](APP_PACKAGES.md) |
| File Explorer | Native userspace browser, file operations, text previews and app launching | [Explorer](EXPLORER.md) |
| Notepad | Native text editing, atomic saving, unsaved prompts and Explorer Open with | [Notepad](NOTEPAD.md) |
| Task Manager | Core 0 CPU history, grouped programs/background/kernel tasks and opt-in Kernel / Input breakdown (ABI 10) | [Syscalls](../APIDocs/Syscalls.md) |
| Images | PNG/JPEG wallpapers; lazy PNG icons, alpha, themes and bounded caching | [Images](IMAGES.md) |
| Image Viewer | Native PNG/JPEG app, Explorer association, fit/zoom/pan, bounded userspace arena | [Viewer](IMAGE_VIEWER.md) |
| Fonts | Runtime TTF selection with built-in fallback; printable ASCII UI | [Fonts](FONTS.md) |
| Storage | Journaled ext4 USB root, persistent apps/settings/files, atomic saves, automatic capacity setup | [ext4](EXT4.md) |
| Input | xHCI HID, USB hubs, descriptor-based pointers, PS/2 keyboard/Synaptics | [USB/input](USB_INPUT.md) |
| Audio | E470 music and volume popup work; Fn volume delivery and FnLock swap confirmed after initialization. Automatic startup probe added; fresh-boot timing awaits hardware check | [Audio](AUDIO.md) |
| GPU | Intel Gen9 composition, page flips and RCS scaling enabled by runtime validation; CPU fallback | [Display roadmap](GPU_DESKTOP_ROADMAP.md) |

ThinkPad E470 headphones work through codec `14f1:5111`, pin `0x16`, DAC
`0x10`, after SKL/KBL controller reset/link setup. Speaker pin `0x17` was
silent despite D0, output enabled, selector 0, unmuted zero-dB DAC gain and
completed DMA; the EC mute gate was the missing step. GPIO data/mask/direction
are zero. The alternate-DAC command
does not change the default route policy. See [Audio](AUDIO.md).

The owner confirmed Windows speakers require a physical volume key after every
boot; the software slider does not restore them. A Windows warm restart into
Wyzar does not preserve working speakers. The current firmware-mute backend
exposes `audio firmware` / `audio unmute`: EC byte `0x30`, bit 6, following
Linux thinkpad-acpi. It requires exact PCI/controller/codec IDs, checksummed
Lenovo ECDT/FADT, validated EC System I/O ports and a FACS Global Lock in ACPI
memory. It preserves every other EC bit, verifies readback, performs no NVRAM
or GPIO writes, and disables itself after an incomplete transaction. Physical
speaker recovery was confirmed by the owner on 2026-11-25. The log shows EC
audio byte `0x40` -> `0x00`, speaker DMA through pin `0x17` / DAC `0x10`, and
a subsequent `0x00` readback. ECDT ports are `0x62`/`0x66`, path
`\_SB.PCI0.LPC.EC`; SCI_EN was zero. Missing resources decline EC access.
Music playback opens that firmware gate automatically; diagnostic tones may
still require explicit `audio unmute`. Physical hotkey delivery is
confirmed for volume-down/up in the later MP3 test, with verified DAC gain
changes. The owner later confirms good sound/no errors and plain F1/F2/F3
(including mute) after the service/buffer/rate fixes. Fn combinations do nothing.
The earlier MP3 log contains three combined
`PCM DMA error/poll deadline missed` failures, which did not distinguish the cause.

PS/2 multimedia make/break events are consumed and queued by IRQ1; the audio
service task (or a blocking PCM operation) applies shared DAC volume/mute. Up/down change
volume by 10 and clear shared mute; the multimedia mute key toggles mute.
The temporary plain F1/F2/F3 audio fallback has been removed, preserving ordinary
function keys for apps. Volume follows firmware FnLock mapping. USB media input
now handles boot keyboard usages and Consumer Control reports; see USB_INPUT.
Each interface owns its report-ID held state and repeat timer. The audio consumer
ticks repeat while USB registry publication/removal is IRQ guarded; unplugging
one keyboard cannot cancel another's or PS/2's held keys. Consumer-only interfaces
are type4 in the keyboard registry, avoiding mouse-slot exhaustion. The four-
interface receiver typing suppression now requires Razer VID 0x1532. USB event
counts are available through `audio firmware`. User target: wired Dareu EK815S,
Fn+4–6; also wants hubs and multiple keyboards. Host media/setup fixtures and
real QEMU USB transfers/hub hotplug pass. The owner confirms Dareu media keys work,
but reports Audio service at 1.2–2.5% idle CPU. Latest code replaces the perpetual
2 ms loop with event-driven blocking and exact repeat/diagnostic deadlines. DMA
keeps 2 ms service only while running; busy syscall owners notify on release.
Key/mixer/startup/USB/capture events wake the worker, with its final predicate
and BLOCKED publication IRQ guarded. The wake hook only interrupts the explicit
service wait, preserving internal hardware sleeps. `process_exit_ui` also
notifies PCM owner departure so idle open streams do not require owner polling.
QEMU idle probes use actual Task layout/accounting and assert stable service
iteration and CPU-cycle counts plus BLOCKED/wake_at_tick=0 before/after activity.
Hardware idle CPU with this change remains to be checked.
The icon redesign was pushed as f2bf09a before this USB work. This is not a general AML or
ACPI HKEY/GPE implementation: the narrow captured-E470 initialization is
described below; SCI/GPE masks are unchanged. Zero key counters may indicate ACPI-only
delivery; the direct unmute command still works when EC resources are available.

`play <path.mp3>` starts `/share/system/mp3_player.elf` as a foreground child using
OPEN_WITH origin=1 (shell); origin=0 retains desktop semantics. The CC0 minimp3
decoder and continuous windowed-sinc resampler are userspace only. Native 44.1/
48 kHz PCM bypasses conversion; action 7 optionally negotiates the requested
rate via its third argument. HDA actions 7–10 own/write/drain/close one stereo
s16 stream; action 6 returns shared volume/mute, 11 sets volume and 12 controls
mute. A bounded 256 KiB DMA
ring clears consumed pages; pointer validation walks user page permissions.
The dedicated audio task services DMA every 2 ms and cleans up dead/interrupted
owners, independently of the USB/input loop. A 128 KiB prefill and 1.2 s service
deadline replace 32 KiB/300 ms; errors log status/position/gap/queue separately. Underflow
stops instead of replaying a stale ring. See AUDIO.md for limits and QEMU tests.

The volume tray icon sits immediately left of the clock. Taskbar popup mode 3
uses the existing opaque retained cutout and content replay machinery, including
stale-clip recovery and both destination indices. The shared AudioMixer publishes
desired volume/mute with a revision; UI callbacks damage only tray/panel, and
the audio task applies hardware changes outside IRQ/UI handlers. Keys, slider,
wheel and syscalls share that state. Up/down repeat at 400 ms then every 80 ms;
break cancels, typematic makes are suppressed, mute never repeats. CPU QEMU UI
tests and the GPU mock fixture cover this path; physical popup testing is pending.

Owner reports GPU Rainbow lag during volume changes even with Boot Log fully
covered. Routine audio-control diagnostics now use memory-only klog output;
ordinary print() unconditionally rerendered the log's backing surface. The
volume UI damages only icon appearance changes and the open panel. Stable
retained popup damage is deferred to the final opaque replay, and tray repairs
are allowed with popups above the taskbar. GPU regression covers a visible log
fully occluded by fullscreen opaque Rainbow, actual queued volume updates,
unchanged log contents and no graphics submissions for panel/tray repair.
Physical performance verification of this fix is pending.

Owner says the GPU volume change seems fine. Fn audio keys now work after
firmware initialization; automatic startup activation is the latest change.
FnLock has no indicator. The attached host is Acer Nitro AN517-54, not the E470.
The owner supplied `G:\fnkeys.bin`; extracted tables/report are under ignored
`.storage-test/e470-tables` and `.storage-test/e470-fn-report.txt`. Initially Fn
combinations emitted only Fn make/break; CMOS/EC status stayed unchanged. The DSDT defines
SAUM(2) as EC byte 3 bits 5–6 = 2. The new build configures that mode on the
first successful exact-E470 audio probe, preserving other bits and verifying
readback under the FACS lock. `thinkpad_hotkey_profile.h` checks captured table
identity/checksum and EC-field/SAUM fingerprints; other firmware is skipped.
The follow-up `G:\fnkeys.bin` (48 events, zero drops) confirms SAUM 0x60->0x40
but still only Fn make/break during combinations; EC/CMOS stay unchanged and
SCI_EN=0. Latest build adds the captured Linux `_SB._INI` branch's immediately
following UCMS(0x1c), using fingerprinted SMI/MNVS fields and methods. Its mailbox
base is extracted from AML and must be backed by ACPI NVS; command is serialized,
skips pending mailbox, and bounds OS retries to 50 triggers/50 ms. No FACS lock
is held across SMI. Host tests pass against the second captured DSDT including
method corruption and relocation cases. The third capture confirms UCMS
status/result zero but unchanged Fn-only trace and SCI_EN=0 (48 events/no drops).
Latest build enters ACPI mode through FADT SMI_CMD/ACPI_ENABLE before SAUM/UCMS,
waiting at most 300 ms for PM1a/b SCI_EN. Only a fingerprinted E470 with unused,
already masked PIC IRQ9 is accepted. IRQ9 remains masked; there is still no
general SCI/GPE/query dispatcher. fn_probe now records SCI_EN (kind6) and EC03
(kind7). The fourth capture confirms SCI_EN=1, EC03=0x40, 65 events/no drops,
and real Fn+F1/F2/F3 multimedia make/break codes. After Fn+Esc, Fn combinations
produce plain F1–F3 while plain F keys produce multimedia codes. The owner
confirms keys work only after starting `audio hotkeys`; lazy initialization was
the remaining startup defect. Latest code publishes `hda::desktop_ready()` after
the first desktop frame (PCI discovery already complete), then the audio service
probes discovered HDA controllers under the existing busy/attempted guard. This
enables the firmware path without any user audio command. No-HDA boot skips the
probe; other platforms retain generic HDA initialization and exact firmware gates.
QEMU audio tests now assert readiness before the first command and no duplicate
probe. E470 fresh-boot activation without diagnostics remains to be verified.
Fourth evidence: `.storage-test/e470-fnkeys-fourth.bin`, `e470-fn-bootlog-fourth.txt`.
Third evidence: `.storage-test/e470-fnkeys-third.bin`, `e470-fn-bootlog-third.txt`.
Older evidence files are ignored:
`.storage-test/e470-fnkeys-new.bin`, `e470-tables-new/DSDT.aml`.
`audio hotkeys` launches a bounded diagnostic helper
and saves `/home/fnkeys.bin` (raw PS/2, E470-only read-only status, FADT/ECDT/DSDT/
SSDT export). It uses SYS_FIRMWARE_DIAG 225 and does not execute AML or consume
EC queries. `tools/read_fn_capture.py` validates/extracts the bundle. Normal
audio initialization, including SAUM setup, precedes recording.

Normal new images contain a 256 MiB ext4 seed and occupy 385 MiB overall.
First boot can provision a larger root on a larger stick. This disk seed reserve
is separate from the recovery archive in RAM.

After the disk shell reaches its first stdin request, unused recovery RAMFS
nodes and archive/fallback-module pages are reclaimed. `/tmp`, `/run` and live
logs remain. Failed disk selection or shell loading retains recovery. Removing
the disk later offlines storage until reboot; saves never switch silently to RAM.

## Decisions and implementation constraints

- A prior owner run (2026-11-22) crashed with heap corruption while opening
  an app over GPU tests. Subsequent logs contain no new crash record. Opaque
  overlap and shadows were acceptable in that round of testing.
  Page allocator reservation/freelist updates now exclude preemption; zeroing
  follows reservation. The compositor uses a 64 KiB stack and static frame
  snapshots; task canaries and heap-header diagnostics are enabled. These are
  stability fixes for observed code risks, not proof of the hardware crash cause.
  Slowdown diagnostics now split presentation copy/overlay/flip elapsed time,
  plus BCS/RCS/flip one-ms sleep counts, actual return time and 2/10-ms outliers.
  Read the `GPU present:`/`GPU sleep:` units and nesting in [TESTING](TESTING.md).
  Alpha draws at all supported sizes now use an isolated optimized SSE2 CPU
  renderer. The owner confirmed smooth SIMD blending then severe lag when the
  former size cutoff selected RCS. The SIMD policy now applies at every size
  when selected with `GPU_RCS_ALPHA=0`. Opaque scaling
  retains RCS. CPU grip/fallback blending shares the SIMD renderer.
  `cpuAlpha`/`alphaKiPx`/`alphaMs` show the selected CPU work. Hardware smoothness
  and CPU cost need validation; no GPU cache policy or workaround was relaxed.
  Client surfaces support opt-in premultiplied alpha (ABI 11, syscall 223).
  Opaque clients preserve their existing format and fast paths. Visible alpha
  clients select ordered scene composition: CPU background, opaque/palette BCS
  clients and scaled/translucent RCS draws at their stack positions, with CPU
  fallback when an engine is unavailable. Rebuild the complete horizontal extent of damaged rows
  before blending; never accumulate alpha onto the retained prior output.
  Translucent clients do not hide underlying animation; their chrome remains
  opaque. The GPU Scaling Test toggles alpha with A. The owner confirms alpha
  works without failures, but observes Rainbow CPU rising from about 5% to 24%.
  Ordered scenes now keep opaque/palette clients on BCS instead of CPU rows.
- Non-maximized windows have cached shadows: shared 169-byte edge/corner alpha
  mask, radius 12 and downward offset 4. The optimized isolated pixel renderer
  blends strips before each window; no per-window shadow allocation or rebuild.
  Shadow damage uses ordered GPU/CPU composition. Retained client updates remain
  enabled away from exposed shadow edges, ignoring shadows hidden by opaque
  foreground windows. Full-window damage includes shadows; lifecycle transitions
  retain full-scene invalidation. The owner confirms shadows work without
  failures, but partially covered Rainbow remains slower/more CPU intensive
  than fullscreen, especially beneath alpha.
- Title-bar pixels are now cached per window with width/height, title, font/icon
  resource epoch, focus, hover, closability and maximize keys. Moves reuse the
  cache; retirement frees it. Metadata has no borrowed resource ownership.
  Cache allocation failure falls back to clipped direct rendering. Hover
  transitions explicitly damage the old/new control even without client damage.
  Normal ordered client damage now stays within its X/Y rectangle, subtracts
  higher opaque coverage, and preserves L-shaped damage while capacity permits.
  Keep shadow/alpha backgrounds and the fullscreen retained fast paths.
  New title interaction and covered-Rainbow performance need hardware verification.
- Owner follow-up: manually fullsized Rainbow ~5% CPU; small opaque GPU test
  above it ~12%+; large opaque test ~2%; A returns to ~12% and Rainbow lags.
  The overlap problem is not confirmed resolved by clipping/title caching.
  RCS now reuses the staged source only for matching immutable publication
  revision/address/size/retirement epoch. WIN_PUBLISH stamps a new unique revision
  under the existing publication IRQ guard. Mutable sources always upload.
  Destination flushes, fences and context retirement are preserved. Boot Log's
  bounded GPU perf samples after layout/alpha changes report elapsed driver
  phases (including sleeps), uploads/reuse and submission counts, not CPU usage.
  Use new physical samples to locate the remaining cost; do not attribute it to
  palette-only remapping, whose helper already preserves window bindings.
- The new physical log confirms upload reuse and shows BCS/presentation elapsed
  time dominating (~30 active compositor loops/s; slow alpha samples ~15/s).
  Compositor one-ms GPU sleeps could previously wait for the normal ten-ms
  quantum because timed wakeups were checked only in schedule(). The timer now
  wakes due PID-1 timed sleeps promptly and invokes existing compositor priority.
  Ordinary ten-ms quanta are preserved, including across early wakes. A wake on
  the currently halted stack restores RUNNING state. GPU commands, coherence,
  retirement and flip synchronization remain unchanged. Measure new hardware
  loop rates/CPU usage; phase logs are elapsed rather than CPU time.
- The subsequent owner log still reaches F ~50 ms (~20 active loops/s) with
  zero source upload bytes, roughly 5-9 BCS submissions/loop and sometimes two
  RCS draws/loop. GPU Rainbow now queues one exposed-region bounding scene and
  relies on GPU-plan opaque clipping, rather than waiting once per exposed
  strip. CPU mode retains separate rectangles; fully hidden animation stays off.
  Ordered alpha/shadow scenes do not split into a separate HUD/top-client pass;
  HUD overlays still draw after client composition. General damage-list behavior
  and opaque retained HUD splitting are preserved. Verify the new batch counts
  and F/CPU costs on hardware.
- Gen9 RCS milestone (2026-11-21): the owner reported 900 seconds without
  freezes on the ThinkPad E470 at its 256 MiB aperture setting, with repeated
  resizing, overlapping windows and software restarts. Surface Laptop 1 also
  passed hardware tests. The preserved v10 image/source/checksums are local at
  .storage-test/checkpoints/gpu-v10-stable-800s-2026-11-21/.
  v10 adds enabled slice/subslice steering, coherent L3 line flushing,
  retry/credit, atomic/eviction and context/TLB workarounds from Linux Gen9.
  Keep these after engine reset and verify the programmed values.
  v11 restores direct clipped draws into the pinned desktop backbuffer and
  sleeps on longer completion waits after a brief spin. The owner reports
  smooth large-window rendering; sustained v11 validation remains ongoing.
  Every draw waits for both its completion fence and context schedule-out.
  Failed desktop submissions quarantine buffers and select CPU presentation.
  GPU_RENDER follows GPU_COMPOSE by default. GPU_TRACE=1 adds stage labels, PCI power snapshots,
  ring-wrap tests and extended isolated resize stress sweeps. GPU_TRACE=0 keeps
  short offscreen texture/scaling/alpha readback checks. Standard test build:
  make iso FRESH=1.
  SCORE marks scoreboard retirement before render-target cache writeback;
  RTCLEAN marks completed render-target writeback before the DC/depth drain.
  The workarounds resolved the reported v10 SCORE reproduction, but the exact
  responsible setting has not been isolated. Do not attribute the stall to
  aperture capacity or NVIDIA power state based only on the prior logs.
  Host GPU tests, boot paths and GPU test-window interactions pass. QEMU uses
  a CPU backend and does not validate Intel GPU execution. F: is frame time
  in milliseconds. The standalone GPU test animation sleeps 33 ms per frame.
- `GPU=1` is now the default after owner validation. Composition follows GPU,
  presentation follows composition, and runtime discovery/readback select the
  supported hardware path or CPU fallback. `GPU=0` forces CPU mode. Keep
  RCS enabled with composition and kernel `OPT=-O0` as defaults. The image codec and isolated
  overlay pixel renderer have their own optimized rules. Test kernel-wide
  optimization changes separately.
- QEMU does not emulate our Intel Gen9 GPU. Keep CPU fallbacks and bounded
  hardware waits; record what was actually tested on physical hardware.
- Apps are files plus metadata. Do not add new applications permanently to
  `wm_init`. Boot Log, Rainbow and Visual Test remain built-in fixtures.
  All terminals use the userspace console host with independent channels.
- Separate parser/state logic from MMIO so host tests can exercise actual code.
- Single-core IRQ guards are not SMP locks. Storage uses a sleeping mutex plus
  short IRQ guards; do not restore interrupt-disabled runtime disk transactions.
- TTY reads block until input, a queued shell launch or cancellation wakes them;
  do not replace them with runnable yield loops. CPU idle intervals end at
  interrupt entry, before a handler can switch tasks, to exclude scheduled work.
- Renderers must not read files or decode images. Resource work publishes
  complete snapshots only after input/compositor readers finish.
- Surface rectangles have exclusive right/bottom edges; the dirty-region
  presenter takes inclusive bounds. Preserve conversions and cursor ordering.
- Rainbow animation derives damage from exposed client regions and skips rendering
  when fully covered/off-screen. CPU mode keeps a complete backing surface when
  exposed. GPU mode uses an immutable palette plus phase and generates CPU-only
  rows directly; do not read its stale full backing as the current image. The
  visibility calculation subtracts opaque clients and translucent-window title bars.
- GPU composition completes before CPU menus/HUD/cursor. CPU presentation is the
  fallback; supported GPU presentation copies the clean master into a hidden GPU
  scanout, overlays its cursor/outline, then arms a synchronous plane flip.
  Never reuse/free a batch's sources before its fence/reset. Failed reset pins
  the old destination and swaps to a reserved CPU buffer before full recomposition.
- Cached GPU source bindings require both matching virtual page ranges and the
  surface retirement epoch. Bump that epoch before the collector frees backing.
  Coherent composition requires private PAT[0] policy setup and the dirty-cache
  boot readback test; do not enable no-flush sharing from a cold-pixel check alone.
  Presentation requires verified uncached GPU writes because display does not
  snoop CPU caches. Its dedicated PMM output buffers have WC-only CPU mappings.
  Reject FBC/PSR and unsupported plane layouts; reuse a buffer only after LIVE
  and the frame counter confirm the other buffer latched. Failed flips restore
  firmware scanout or retain the pinned live private buffer for CPU fallback.
- Default GPU rendering adds a private logical RCS context and PPGTT.
  Scaled client draws use the pinned master buffer with explicit cache handoffs.
  Geometry, sampling, ring reuse, reset preparation and Gen9 workaround tests
  run on the host. Preserve source/destination lifetime through retirement and
  keep font/icon rasterization on the CPU. Alpha clients use ordered RCS draws
  with the same explicit cache handoffs and retirement waits.
- Kernel string helpers are local definitions, not a hosted C runtime.
  Compiler-generated libc calls and the ext4/codec shim need deliberate linkage.
- Interrupt and syscall entry preserve x87/SSE state in aligned stack frames
  before calling C++. Kernel entry uses default floating-point controls; exit
  restores the interrupted controls/registers. Keep this ahead of C handlers:
  optimized UI drawing and the compositor both use XMM registers. AVX is not
  enabled; enabling it requires broader extended-state preservation.
- Do not remove USB protocol fixes as speculative workarounds. Preserve bounded
  reset/probe handling and exact transfer-completion ownership.
- Headers are dependency-tracked; compiler-option changes require `make clean`.
  Close QEMU before updating an image it has open.

## Known limitations

- ELF validation, segment permissions/W^X, user-pointer checks, window-ID
  authorization and malformed-ELF/failure cleanup remain incomplete. Normal
  user-task reap releases loader-owned pages and private page tables. The app API
  is for trusted programs, not a finished security boundary.
- One foreground application per console channel; no process groups, general
  signals, argument/environment passing or dynamic linking.
- No client present acknowledgments, hardware cursor plane, general modesetting or
  GPU client API. Experimental RCS support requires physical readback validation.
  PNG/font alpha works on the CPU already.
- Stable boot-volume identity, GPT, internal-drive drivers
  and installation remain pending. Permissions, links and UTC file timestamps
  also need defined semantics.
- External FAT32/exFAT file transfers, progress/cancel, timestamps and eject are
  implemented. Runtime timezone/DST configuration is pending. Icon
  themes implement the native subset in [IMAGES](IMAGES.md).
- A failing device can delay sibling HID discovery during synchronous
  enumeration. Surface/ZM3310 wireless stutter is not confirmed resolved.
- Some generated binaries/images are tracked. `.gitignore` does not untrack
  them; review source/docs separately and avoid committing build outputs.
  Keep line-ending normalization separate from functional edits.

## Verification boundary

On 2026-11-19, CPU, `GPU_COMPOSE=1 GPU_PRESENT=0` and `GPU_PRESENT=1`
production kernels built in WSL. The production page-flip image contains no
software test backend and passed ISO, USB and RAM-recovery boot/shell checks.
The GPU host command interpreter passed ASan/UBSan tests. A separate test-only
CPU backend in QEMU verified current rainbow pixels, skipped full CPU generation,
and partial-batch failure with buffer quarantine, source pinning and CPU recovery.
None of these emulator checks execute Intel MMIO or validate physical image
correctness, vblank latching, cache visibility or reset behavior.

The owner's first `GPU_COMPOSE=1` laptop log identifies Intel `0x5916`, a 4 GiB
GGTT and a 256 MiB aperture. Initialization failed before engine startup because
the allocator found no sufficiently large non-present interval. GGTT allocation
now also checks long uniform firmware scratch aliases outside protected display
ranges. The owner subsequently saw `opaque+rainbow`, but reported GPU CPU usage
around 24% mostly covered (CPU mode 8%), 6% hidden, and up to 80% fully exposed.
The updated revision configures/verifies coherent LLC sharing and reuses source
bindings, avoiding bulk pixel-cache flushes and repeated MMIO mappings. It also
keeps composition-mode boot tests offscreen. The owner subsequently confirmed
about 8% CPU mostly covered and 24% fullscreen versus 60% fullscreen on CPU.
The new experimental `GPU_PRESENT=1` stage and fullscreen retained damage path
have host/mock tests. After fixing the SURFLIVE address comparison, the owner
reported about 5% CPU fullscreen and no visible tearing. Warm-restart init
failure is suspected, pending a failed-boot log. They add same-mode
page flipping, not general modesetting or shader rendering. Do not describe the
earlier allocation failure as unsupported hardware,
insufficient VRAM or a GPU hang.

Earlier desktop/compositor behavior and the Gen9 blitter ran on the test laptop.
The owner confirmed that the discovery grace window restored Surface USB-C root
mounting, and that automatic capacity setup and `mv` worked on a physical 4 GB
stick. These observations do not validate every later driver/desktop change.

Recent host/QEMU checks cover ext4 save/recovery/failure paths, capacity setup,
USB protocol helpers, PNG/JPEG resources, lazy icon lifetimes, recovery-memory
reclamation and default-VGA ISO/USB/recovery boot. The combined build still needs
physical input, persistence, power-action and display verification. See
[TESTING](TESTING.md); do not generalize one result to an entire hardware class.

## Maintaining these docs

Update feature guides when behavior changes, this snapshot when architecture or
constraints change, and the pending-work list when work completes. Investigation
chronology belongs in version-control history. Preserve unresolved limitations
and test evidence instead of appending superseded status entries.
