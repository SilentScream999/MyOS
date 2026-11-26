# Music Player

Music Player is a native userspace app (`/bin/music_player.wiz`). Open it from
Start, run `run /bin/music_player.wiz`, or double-click an MP3 in Explorer.
Explorer's Open with menu also offers Music Player (M).

Use Open or Ctrl+O to choose an MP3 with the common file dialog. Playback starts
automatically. Space toggles pause/resume, S stops, and Play restarts the current
track after stopping or finishing. M toggles app mute; +/−, the mouse wheel,
and the draggable volume bar change this player's output only. The app level
is retained when stopping, opening another song, or replaying. Hardware volume
keys and the tray mixer control the independent system master level.

The app supports MPEG Layer III mono/stereo at 8–48 kHz. It streams files through
a 64 KiB input buffer, skips ID3v2 metadata, and uses the existing minimp3 decoder
and continuous windowed-sinc resampler. Native 44.1/48 kHz playback bypasses
conversion when supported by the output. There is no whole-song allocation.
This first version has elapsed playback time, open/play/pause/stop, and volume;
it does not provide seeking, playlists, cover art, or other audio formats.

Rendering uses NativeSurface, shared native vector controls, runtime UI fonts,
immutable WIN_PUBLISH snapshots and the desktop compositor's normal CPU/GPU
paths. Only dirty frames publish. The icon extends the existing geometric PNG
authoring system with a shaded navy record and warm play badge at all seven
theme sizes. The Open glyph aligns with its label; taskbar and popup speaker
icons use smaller artwork while retaining their existing click targets.

The app owns the regular SYS_AUDIO PCM stream. ABI 12 adds partial nonblocking
writes, pause/resume preserving DMA position, queued-byte queries/start and
consumed-byte queries. These keep decoding/submission incremental so window and
picker events remain responsive. The audio service continues DMA and multimedia
keys independently; a paused stream has no periodic DMA deadline. The existing
`play <path.mp3>` console player remains available. One stream can own the
output at a time; a busy output is reported with a retry instruction.

ABI 13 adds owner-scoped stream volume/mute (SYS_AUDIO action 17). The worker
combines app and master levels without modifying the shared mixer, and applies
hardware DAC attenuation where supported. Codecs without DAC gain use software
scaling for newly queued PCM; volume changes there can lag by the buffered audio.
Stream settings reset at open, and the player reapplies its retained app level.
Mouse-move events report held buttons so a release outside the client cancels
the slider drag on reentry.

Build and validate in WSL/Linux:

```sh
make test-music-player test-explorer test-audio test-wiz
make iso
python3 tests/qemu_mp3_test.py
python3 tests/qemu_music_player_test.py
python3 tests/qemu_volume_popup_test.py
```

The host app test uses the actual decoder and native renderer with a simulated
syscall backend. It checks partial writes, backpressure, pause/resume, drain,
cleanup, invalid data and small-window rendering. It writes a preview under
`.storage-test/music-player.png`. The diskless QEMU GUI test checks real PCM
consumption, pause past the service deadline, an idle paused worker, the common
picker, resume, stop/replay and close cleanup. Physical sound and device
pause/resume still require a hardware check.

Verified on 2026-11-25 in Ubuntu WSL: sanitizer player/Explorer/audio/package
tests, the full ISO build, the diskless GUI suite and the existing console MP3
suite all passed. The GUI check caught status queries waking the paused audio
worker; counter snapshots now leave it asleep. The normal ISO and persistent
USB image were then refreshed with the tested app.
