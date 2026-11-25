# Audio playback and hardware support

The first audio milestone is a shared PCI High Definition Audio (HDA) driver
with analog stereo test output. The physical target is the ThinkPad E470.
ThinkPad E470 wired headphones and speakers are confirmed working with test tones.
The terminal now launches a userspace MP3 player over an exclusive HDA PCM
stream. The owner confirmed physical MP3 playback and volume-down/up keys on
the E470. The owner then confirmed the dedicated-service/larger-buffer/native-rate
build sounds good with no errors, and plain F1/F2/F3 work. Repeat and the tray
popup work for the owner. The fourth hardware capture confirms ACPI mode and
Fn+F1/F2/F3 multimedia make/break delivery, including the mapping swap after
Fn+Esc. The owner confirms the keys work after `audio hotkeys`, revealing a lazy
initialization dependency. The latest build initializes discovered HDA devices
in the audio service after PCI discovery and the first desktop frame, so the
firmware key setup no longer requires playback or a diagnostic command. This
automatic startup timing change still needs E470 verification.

## Play music

Copy MP3s to a mounted drive or Wyzar's persistent root, then run:

```text
play /mnt/Kingston/Music/song.mp3
audio status
audio volume 70
audio mute
```

`play` treats everything after the command as the path, including spaces; do
not add quotation marks. The bundled `/share/system/mp3_player.elf` runs as a foreground
child so Ctrl+C stops it and returns to the terminal. Playback streams the file
through a bounded input buffer instead of loading the entire track into RAM.
It supports mono/stereo Layer III MP3 (including VBR), skips initial ID3v2 tags,
and plays 44.1/48 kHz PCM at its native rate when the DAC advertises support.
Other supported 8–48 kHz rates use a continuous 32-tap windowed-sinc resampler
with interpolated phases, replacing the first build's linear interpolation.
Native-rate PCM bypasses resampling entirely. The decoder is the pinned CC0
minimp3 source in `third_party/minimp3`;
its license is bundled in `/share/minimp3-license.txt`. Rate changes inside one
file are rejected. This first player has no seek, playlist, pause, gapless trim
or GUI. WAV and mixing remain future work.

On the supported E470, opening playback automatically clears the verified EC
mute gate. Automatic routing prefers headphones if jack presence is reported,
then speakers, headphones or line out according to available routes. Plug in
headphones before starting a track; live jack switching is not implemented.

Fn+F1/F2/F3 (mute/down/up) are handled when firmware delivers PS/2 multimedia
codes. On the captured E470 firmware, the startup audio probe now requests ACPI
mode through the FADT enable command, retaining the masked SCI IRQ, then sets the
firmware's `SAUM(2)` software-key mode and verifies readback, then performs its
paired Linux-initialization `UCMS(0x1c)` call. Key delivery in that build is
hardware verified after initialization. The temporary plain-F1/F2/F3 fallback
has been removed: ordinary function-key scan codes pass through to applications.
Volume follows whichever row combination the firmware maps to multimedia codes,
according to FnLock. USB consumer-control reports are not yet supported.
Each press changes volume
by 10 percentage points; up/down also clear software mute. Holding up/down
repeats after 400 ms, then every 80 ms; release cancels repeat and delayed
service never generates a catch-up burst. Mute toggles once per press.
Volume defaults to 70% and is shared across apps until reboot.
Mute retains the volume level. These controls work while music plays and when
another window has focus. The verified DAC output amp changes gain without
rewriting queued PCM; pins are disabled for mute/zero volume. Codecs without a
DAC output amp use scaling as samples enter the ring, with queued-audio latency.
`audio status`, `audio volume 0..100` and `audio mute` provide terminal controls.

Click the speaker icon immediately left of the clock to open the Volume popup.
Click/drag the slider, click its speaker button to toggle mute, or use the wheel
for 5-point volume steps. Dragging captures the pointer until release and clamps
at 0/100, even outside the panel. Escape or an outside click closes the popup.
Keys, terminal controls, the slider and tray icon share one mixer state; codec
updates are deferred to the audio task so UI/IRQ handlers never sleep on verbs.
Fn-only firmware events still require separate ACPI/HKEY support.

The opaque volume panel is taskbar popup mode 3. It uses the app picker's panel
layout/cutout, retained animation, full clipped content replay and alternating
destination repair machinery. It does not disable the retained popup path.
Volume changes damage the tray and panel; opening/closing uses existing shell
popup scene invalidation. The same renderer handles CPU and GPU destinations.

GPU volume-lag follow-up: routine key/gain diagnostics append only to the memory
boot log (`open bootlog`), because ordinary `print()` synchronously redraws the
Boot Log backing buffer even when other windows fully cover it. Volume changes
damage only the speaker cell when its mute/wave appearance changes, plus the
open volume panel. A stable retained popup's content damage is handled by its
final opaque replay, without rebuilding underlying windows. The tray's exact
repair remains available while an opaque taskbar popup sits above it.
The new covered-log/repair regression passes in the GPU mock; physical frame
timing still needs an E470 retest.

The kernel validates PCM user pages, limits writes to 64 KiB, permits only the
owning PID to write/drain/close, and pins a 256 KiB cyclic DMA buffer. Consumed
pages are cleared before reuse. Playback stops on underrun, DMA errors, missed
poll deadlines or owner exit/interruption. Only one stream is supported; a
second player or diagnostic tone is rejected while a stream is open. A dedicated
audio task services the ring every 2 ms, independently of USB/input enumeration.
The 128 KiB prefill gives about 0.68 seconds of queued audio at 48 kHz; a full
usable ring holds about 1.34 seconds. The conservative poll deadline is 1.2 s.
Errors now distinguish descriptor DMA, hardware FIFO, invalid position and
missed service deadline, logging status, position, service gap and queued bytes.
The original combined error does not establish which of these failed on E470.

Linear interpolation can attenuate high frequencies; see the author's
[frequency-response derivation](https://www.dsprelated.com/freebooks/pasp/Linear_Interpolation_Frequency_Response.html).
That makes it a plausible contributor to the reported dullness, not a confirmed
diagnosis of the whole analog sound path. The host quality test checks 15 kHz
amplitude after 44.1-to-48 kHz conversion as well as exact native-rate samples.

Validation: `make test-audio` exercises stream accounting, resampling duration
and interpolation, volume bounds, EC protocol and topology under ASan/UBSan.
`python3 tests/qemu_mp3_test.py` generates its own stereo/mono MP3 fixtures using
host libmp3lame, boots a disposable diskless test ISO and records decoded PCM.
It checks resampled frequencies, live mute, key handling, ID3 skipping, missing/
malformed files, cancellation and reopening. `qemu_audio_test.py` and
`qemu_audio_timeout_test.py` cover diagnostic playback and command recovery.
`qemu_volume_popup_test.py` checks the production CPU icon/panel, slider endpoints,
drag capture, shared mute/key state, hold/release, Escape and outside dismissal.
The GPU compositor fixture additionally corrupts the volume panel and verifies
all pixels after retained replay on both destination indices with stale clips
and changing slider/mute state. This mock test does not verify physical Intel GPU timing.
It also puts opaque fullscreen Rainbow over the still-visible Boot Log, sends
real volume updates through the audio queue and verifies the log buffer stays
unchanged and popup/tray repairs submit no additional GPU work.

## E470 firmware mute backend

### Capture missing Fn-key events

The next Fn step needs the E470's actual firmware tables and key-delivery data.
FnLock works under Windows according to the owner, but the laptop has no FnLock
light, so its state in Wyzar is unknown. The current build adds a bounded capture
command:

```text
audio hotkeys
```

Follow the ten on-screen prompts (about 30 seconds): Fn+F1/F2/F3, Fn+Esc,
Fn+F1/F2/F3 again, then plain F1/F2/F3. The foreground helper saves one file,
`/home/fnkeys.bin`. Copy that file to the external drive using Explorer and attach
it for analysis. Ctrl+C cancels and disarms recording. Fn+Esc may have toggled
FnLock; toggle it back if desired.

The explicitly armed capture stores up to 512 raw PS/2 bytes/changed firmware
samples with stage/time markers. On the exact E470 identity it samples the
documented ThinkPad mixer status at CMOS `0x6e` (Linux NVRAM offset `0x60` plus
14), EC event flags and read-only EC audio byte `0x30`, with existing bounded
protocol/Global Lock handling. It does not consume EC queries or change CMOS,
SCI or GPE configuration. The normal audio probe runs before recording, including
the firmware-profiled SAUM setup described below. On other machines only raw keys and tables
are captured. Normal use never enables this recorder.

The November 25 E470 capture has no dropped events: Fn combinations emit only
`E0 63` / `E0 E3` (Fn make/break); plain F1–F3 emit their standard scan codes.
CMOS 0x6e, EC event flags and EC 0x30 do not change. Its checksummed DSDT is
LENOVO `TP-R0D  `, revision 0x960, length 78213. Its `SAUM` method assigns
`HAUM`, an ECOR byte-3 field at bits 5–6; mode 2 is also used by this firmware's
Linux initialization branch and by Linux thinkpad-acpi's software-mute setup.
`thinkpad_hotkey_profile.h` fingerprints the EC declaration/field prefix and
complete SAUM method at their captured offsets, in addition to checking table
identity, length and checksum. Unrecognized firmware is skipped. The exact E470
PCI/codec identity and validated ECDT/FACS resources remain required.
The setup preserves all other EC byte-3 bits, avoids redundant writes, and
verifies the full-byte readback under the firmware Global Lock. Partial EC
transactions disable the backend until reboot. It neither changes FnLock nor
enables SCI/GPE routing, and no EC query events are consumed. New PS/2 volume
events use the existing queue, mixer, popup and software repeat handling.
The follow-up capture confirms EC 0x03 changed 0x60 to 0x40 and EC 0x30 was
unmuted, but the same Fn-only key trace remains (48 events, zero dropped;
SCI_EN=0). SAUM alone is therefore insufficient on this boot path.
The firmware's Linux `_SB._INI` branch pairs it with `UCMS(0x1c)`, implemented
by `SMI(2,0x1c,0,0,0)`. The new build reproduces that exact additional call once,
after releasing the EC/FACS lock. It separately fingerprints the MNVS region
declaration, mailbox fields, complete SMI and UCMS methods and Linux call site.
The physical MNVS address is read from the validated declaration rather than
hardcoded, and the entire declared region must reside in Limine ACPI NVS.
The 21-byte mailbox at MNVS+0xfc0 is serialized with the backend's busy guard;
an already pending command is skipped. Firmware retries are limited to 50
triggers/50 ms of OS waiting, with no further calls after failure this boot.
As with any SMI, firmware execution inside the trigger cannot be timed out by
the kernel. The call's purpose is inferred from its place in this firmware's
Linux initialization; it has not yet established Fn key delivery on hardware.
Its status/result and skip/failure reason are logged explicitly. Host tests
cover exact arguments, relocated mailbox addresses, changed AML rejection,
delayed completion, timeout, firmware error and busy-mailbox preservation.
The third capture confirms UCMS status/result both zero at mailbox 0xbbe8ffd8,
yet still no volume scan codes (48 events, no drops), with SCI_EN still zero.
Latest code therefore performs the standard FADT ACPI ownership transition
before the SAUM/UCMS initialization, only on this fingerprinted E470 and only
with unhandled PIC IRQ9 already masked. It sends FADT ACPI_ENABLE (captured
value 0xf0) to SMI_CMD (captured 0xb2), then waits up to 300 ms for SCI_EN in
both declared PM1 control blocks; it never forces SCI_EN by writing PM1_CNT.
IRQ9 stays masked and no pending GPE/fixed events are acknowledged or consumed.
The kernel still lacks general ACPI event/thermal/power-button dispatch; this
is a targeted polling bring-up path, not full OSPM. Captures additionally record
SCI_EN (kind 6) and EC mode byte 3 (kind 7), so an unsuccessful next test can
distinguish transition failure, overwritten mode and pending EC events.
Host tests exercise delayed/already-complete transitions, missing resources,
timeout, PM1b acknowledgement and unchanged PM1 bits. This transition follows
[ACPI's legacy/ACPI selection protocol](https://uefi.org/specs/ACPI/6.5/04_ACPI_Hardware_Specification.html).
The fourth capture establishes physical success after initialization: 65 events,
zero drops; SCI_EN=1, EC03=0x40, and Fn+F1/F2/F3 emit E0 20/A0, E0 2E/AE,
E0 30/B0. After Fn+Esc, the combinations emit ordinary F1–F3 codes and plain
F1–F3 emit those multimedia codes, demonstrating the firmware FnLock swap.
EC SCI_EVT remains pending (0x20); this driver does not consume or interpret it.
The owner confirms that starting the diagnostic was previously necessary to
activate these controls. The startup fix runs `control_legacy(0,0)` from the
audio task once the compositor publishes desktop readiness, rather than waiting
for that command. PCI discovery is complete before publication, and the existing
busy/attempted guard serializes explicit probes and bounds failed initialization
to one attempt. Machines without HDA skip the background probe; generic HDA
devices initialize normally, while the firmware quirk keeps its exact identity
and firmware gates. QEMU tests require initialization before any audio command,
one initialization after repeated commands, and no E470 setup on a generic guest.
Linux reference:
[software mute setup](https://github.com/torvalds/linux/blob/v6.8/drivers/platform/x86/thinkpad_acpi.c).

The bundle includes checksummed FADT/ECDT/DSDT and up to 32 SSDTs, copied only
from validated ACPI memory. Other tables, including MSDM, are excluded. The host
tool validates and extracts it with fixed table filenames:

```sh
python3 tools/read_fn_capture.py /path/fnkeys.bin --extract-dir .storage-test/e470-tables
```

`make test-audio` tests capture bounds and expiry. `qemu_fn_capture_test.py`
checks real PS/2 delivery, all stages, export checksums, the saved bundle,
unsupported-platform firmware-I/O exclusion and cancellation. QEMU cannot
identify the physical E470 Fn delivery path.

Research references: [Linux ThinkPad hotkey/HAUM/SAUM implementation](https://github.com/torvalds/linux/blob/v6.8/drivers/platform/x86/thinkpad_acpi.c)
and [Linux NVRAM addressing](https://github.com/torvalds/linux/blob/v6.8/drivers/char/nvram.c).

### Confirmed mute gate

The owner confirmed that Windows needs a physical volume-key press after each
boot; moving its software slider does not restore speaker sound. Restarting
from Windows into Wyzar still leaves the speakers silent. The current build
adds a narrow firmware-mute backend. On 2026-11-25 the owner confirmed audible
speakers after `audio unmute`: the boot log shows EC byte `0x30` changing from
`0x40` to `0x00`, followed by completed speaker DMA through pin `0x17` / DAC
`0x10`. A subsequent firmware query still reads `0x00`. ECDT supplies ports
`0x62`/`0x66` and path `\_SB.PCI0.LPC.EC`. The PS/2 multimedia key count is
zero in this earlier run; the later MP3 run records physical volume-up/down
events and verified DAC gain changes. Physical mute-key behavior still needs confirmation.

```text
audio firmware
audio unmute
audio test speaker
audio firmware
open bootlog
```

`audio firmware` reads the documented ThinkPad EC audio byte `0x30` and logs
its mute bit (bit 6), the firmware EC ports/path, FADT SCI information and the
number of PS/2 multimedia key presses received. `audio unmute` clears only bit
6 and verifies the entire byte by reading it back. Every other bit, including
the volume nibble, is preserved. A clear EC bit is not proof of audible speakers.
Save both `Audio:` and `ThinkPad audio:` entries with the physical test result.

The backend requires all of controller `8086:9d71`, subsystem `17aa:505b`, codec
`14f1:5111`, a checksummed Lenovo FADT/ECDT with valid System I/O EC resources,
and a validated FACS Global Lock backed by ACPI memory. It takes the firmware
Global Lock across the read/modify/write, uses bounded sleeping protocol waits,
and signals firmware through GBL_RLS on release when required. Unexpected OBF
data is never drained. An incomplete EC transaction or mismatched readback
disables the backend until reboot. Missing resources produce diagnostics and
no guessed-port fallback. NVRAM and codec GPIOs are not written.

Extended PS/2 Set 1 volume-up/down keys (`E0 30` / `E0 2E`) adjust shared volume;
mute (`E0 20`) toggles shared mute. These events are queued in IRQ1 and executed
by the audio service task or by a blocking PCM operation in task context.
Ordinary F1/F2/F3 are not intercepted for audio. The queue
logs received/dropped keys and does not leak multimedia codes into terminal
text. Firmware unmute is performed when opening E470 music playback.

Wyzar still has no general AML interpreter or ACPI HKEY/GPE event dispatcher.
Its narrow captured-E470 backend now reproduces SAUM(2) and the paired Linux
UCMS initialization, preceded by the FADT ACPI mode transition with the SCI
IRQ kept masked; GPE masks remain unchanged. If the E470 delivers
these keys only as ACPI events, use `audio unmute`
directly; zero PS/2 key counts distinguish that case. If ECDT/FACS are absent,
the backend declines access and a wider ACPI integration is still needed.

References: [Linux thinkpad-acpi mute-byte and HAUM/SAUM handling](https://github.com/torvalds/linux/blob/v6.8/drivers/platform/x86/thinkpad_acpi.c)
and [ACPI EC transactions and Global Lock ownership](https://uefi.org/specs/ACPI/6.5/12_Embedded_Controller_Interface_Specification.html).
Host ASan/UBSan tests exercise byte-protocol ordering, each failed wait stage,
all 256 original audio-byte values, readback mismatch, identity gates, Global
Lock state transitions and multimedia queue behavior. QEMU checks playback,
unsupported-platform rejection and the PS/2 dispatch path; it does not emulate
the E470 EC or establish physical speaker operation.

First ThinkPad E470 result: GCAP `0x9701`, codec mask `0x5` (addresses 0
and 2), timeout on codec 0's initial vendor-ID query (`0x000f0000`). This
establishes controller discovery, but neither codec identity nor analog output.
The follow-up build adds DMA cache flushing, command-ring diagnostics and
reset-before-retry recovery through the optional immediate-command interface.
Physical confirmation of this recovery is pending.

The next ThinkPad log confirms CORB consumed the command (write/read both 1),
RIRB received no response, and immediate commands also time out on codec 0.
Codec 2 responds as `8086:280b` (Intel display audio), but has no analog output.
The external-codec/link failure remains unresolved; speaker routing is untested.
The current build adds Skylake/Kaby Lake reset clock-gating handling and
external-link initialization. Its effect on the ThinkPad is pending testing.

The following physical test successfully enumerates codec `14f1:5111`
(Conexant CX20753/4). Headphone pin `0x16` through DAC `0x10` is audible.
Speaker pin `0x17` through the same DAC completes test DMA but is silent.
The owner reports a boot-menu speaker beep also being absent and Windows
speakers working only after a volume-up key; Fn+F3 in Wyzar does not resolve it.
Another mute/volume gate is a possibility, not a confirmed cause. The speaker
pin reports no standard EAPD capability. The current follow-up explicitly selects
single-input connections, adds 50 ms amplifier settling, and logs supported
route control readbacks before/after configuration. GPIOs are read only when
advertised; no guessed firmware/EC or GPIO writes are made.

The route readback test confirms speaker pin `0x17` and DAC `0x10` reach D0,
pin control is `0x40`, connection select is 0, and both DAC amp channels read
`0x4a` (unmuted at the advertised zero-dB offset). All three advertised codec
GPIOs are disabled, with data/direction zero. Speakers are still silent. These
values do not establish that the physical speaker amplifier is active.
The current build adds `audio test speaker alt`: keep the same speaker pin
but search for a different reachable PCM DAC. This is an explicit experiment,
not an automatic Conexant quirk. If no alternate exists, it returns without
changing hardware. The logs now show each pin's expanded connection list and
the DAC converter stream/channel and format. Default playback still uses the
original first reachable route; headphones retain their working default path.

## Try it on the ThinkPad

Build with `make iso` in WSL/Linux and boot the updated USB image. A normal
image update preserves its saved files; flashing that image onto a stick still
replaces the stick's contents. Existing physical sticks need their boot files
updated or the rebuilt image flashed before these commands are available.

Run in a Wyzar terminal:

```text
audio
audio test speaker
audio test speaker alt
audio test headphone
open bootlog
```

Run the speaker test without headphones, then plug wired headphones into the
3.5 mm jack for the headphone test. Each test produces about 0.8 seconds of a
quiet 480 Hz triangle wave, signed 16-bit stereo at 48 kHz. `audio test` chooses
speakers first, then headphones, then line out; this milestone does not
automatically detect insertion or switch outputs. `audio test line` explicitly
selects line out on desktops/emulated codecs.

`audio` reports routes discovered from codec capabilities and firmware pin
configuration. A reported route is not proof that a physical amplifier works.
The test reports DMA completion separately from hearing the tone. Record
whether speaker and headphone tests are audible, whether a repeated test works,
and any glitches. Boot Log's `Audio:` entries include PCI IDs, controller
capabilities, codec IDs, pin defaults/capabilities, selected pin/DAC, final DMA
position and stream status. Preserve these entries for failed or silent tests.
For speaker diagnosis, preserve both `route state` blocks: widget capabilities,
actual power, selected connection, pin control, amplifier capabilities and
left/right mute/gain readbacks, and advertised GPIO state.
Audio probing is lazy: a normal boot records PCI HDA candidates and maps their
register pages before user address spaces are created; the first `audio`
command initializes the hardware. Failed probes are not retried until reboot.

## Driver boundaries

`src/drivers/hda.h` handles PCI controller setup, D0 power, bounded reset waits,
CORB/RIRB codec commands, widget discovery, analog route configuration and a
pinned cyclic DMA test buffer. It polls with scheduler sleeps and leaves global
controller interrupts disabled; it does not claim a shared PCI IRQ handler.
Only one audio command runs at once. Ctrl+C is checked during playback, and the
stream is stopped before returning. DMA memory remains pinned on failure;
failure to stop the stream disables PCI bus mastering.
Command DMA pages, submissions, responses and playback buffers are explicitly
flushed for firmware configurations where DMA snooping may be disabled. A ring
timeout logs write/read pointers, engine controls/status and the DMA address.
During codec probing, it resets the link and retries the codec with immediate
commands, with both ring engines disabled. A still-unresponsive codec is skipped
after another reset before probing the next advertised address. Late responses
are never reused across transport changes. Playback failures do not switch
transports because resetting would invalidate the active route.
For Intel PCI IDs `8086:9d70`, `8086:9d71`, `8086:a170` and `8086:a171`, reset
temporarily disables CGCTL miscellaneous clock gating (PCI `0x48`, bit 6), arms
codec wake during reset, and restores both prior settings on success or failure.
It walks the bounded extended-capability list to find Multiple Links capability
ID 2. Legacy external link 0 is brought to the acknowledged powered-up state;
if firmware left it at 6 MHz, the driver selects an advertised alternate clock
(preferring 24 MHz) while the link is powered down. No clock change occurs until
SPA/CPA agree. It leaves other links alone and rejects nonlegacy extended-link
layouts. New logs show CGCTL, the capability offset, external-link capabilities,
codec identifiers and before/after control values. QEMU and unrelated PCI
controllers use generic reset.
The older QEMU 6.2 stream-reset behavior is accommodated only when both its
controller ID and virtual codec vendor match; real hardware still requires
stream-reset assertion acknowledgement.

`hda_topology.h` parses both short and long connection lists, expands ranges,
searches bounded routes with cycle detection and rejects digital/input paths.
Limits are four PCI controllers, 32 expanded connections per widget and 16
widgets per output path. The first codec/function group with a usable analog
route is selected. Unsupported formats, oversized/malformed lists and missing
analog paths do not advertise playback. Amplifier gain uses the codec's bounded
zero-dB offset, and the sample amplitude remains low. Other analog output pins
are disabled for an explicit test.

The architecture is shared controller support plus codec support, with small
machine-specific routing/amplifier quirks only when hardware evidence requires
them. There are no ThinkPad-only codec IDs or guessed vendor register writes.
DSP/SOF, SoundWire/I2S, vendor amplifier sequences, HDMI/DisplayPort, USB audio,
Bluetooth, recording and surround are outside this milestone. These require
additional backends or codec/platform work; HDA discovery alone does not cover
every audio device in the owner's fleet.

Implementation reference: [Intel HDA specification, revision 1.0a](https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf),
controller registers/DMA in chapters 3–4 and codec verbs/topology in chapter 7.

Intel platform reference: Linux's
[HDA Intel initialization](https://github.com/torvalds/linux/blob/v6.8/sound/pci/hda/hda_intel.c)
(`hda_intel_init_chip`, `intel_init_lctl`) and
[HDA register definitions](https://github.com/torvalds/linux/blob/v6.8/include/sound/hda_register.h)
for capability headers and legacy multi-link control fields.

## Validation

```sh
make test-audio
make iso
python3 tests/qemu_audio_test.py
python3 tests/qemu_audio_timeout_test.py
```

The host test runs the actual connection parser and route search under
ASan/UBSan, including malformed ranges, limits, cycles and digital rejection.
Immediate-command tests cover stale result clearing, busy/response timeouts,
unsolicited responses and mismatched codec addresses without altering output
on failure.
Link tests exercise the actual capability walker and power/clock logic,
including cyclic/out-of-range headers, existing clocks, advertised rates,
unacknowledged power transitions and extended-link rejection. QEMU does not
emulate this Intel link capability or prove physical external-codec operation.
The QEMU suite boots RAM recovery without any system disk attached, checks the
no-controller fallback, probes `intel-hda`/`hda-duplex`, repeats playback and
checks captured WAV samples for stereo equality, amplitude and 480 Hz period.
It also exercises an unavailable speaker route followed by another valid test
from a second terminal/address space.
The timeout suite redirects the first ring command to an absent codec using
QEMU's debugger, then checks bounded failure, link reset, immediate-command
enumeration and playback. The regular suite places its codec at address 2.
Captures are under `.storage-test/audio-*`. Emulator results do not establish
physical speaker amplification. Headphone operation is separately confirmed
by the owner on the ThinkPad.

For interactive emulation, `make run-audio` attaches an HDA device using a
silent backend. Set `QEMU_AUDIO` to your host's audio backend, for example
`make run-audio QEMU_AUDIO='-audiodev pa,id=audio'` when PulseAudio is available.
Normal `make run` retains its existing device configuration.
