# Native text input

Include `user/native_text.h` for the common insertion caret and single-line
editing component. Notepad, Explorer and the common file dialog use these.

`NativeTextCursor` owns the shared one-pixel caret and blink timing. In a native
app's 16 ms loop, call `cursor.animate(surface, window, frame, active, dirty)`
before the normal repaint and set `dirty` if it returns true. Call
`cursor.reset(frame)` on input or a caret move and mark the view dirty, then
`cursor.draw(surface, x, y, height)` after drawing text during that repaint.
The caret saves its underlying pixel column (up to 64 pixels tall), so idle
blinking changes and damages only that column through syscall 204. Pending
repaints invalidate the saved column; larger carets fall back to a normal
repaint. Inactive fields hide their caret. Notepad keeps its document selection
and multiline navigation and uses this same caret renderer.

`NativeTextInput` adds an insertion position, left/right, Home/End, Backspace,
Delete, bounded insertion/paste, and horizontal scrolling for ASCII fields.
Keep one instance per field. Pass the field buffer, capacity (including its
terminating zero), and the owner's replace-selection flag to `key` or `insert`.
Set the flag on focus or Select All; typing replaces the contents, and left/right
collapses the selection to the beginning/end. Handle application shortcuts
before forwarding printable keys. Use `input.draw(...)` for field text and
caret together, and animate `input.cursor` in the app loop. Avoid marking the
whole view dirty just because `cursor.tick` changed visibility: doing so
redraws the toolbar and file list on every blink.

The kernel shell terminal has a separate two-pixel underline, blinking every
500 ms while the shell owns input and its window is focused. Output/navigation
restarts its visible period. Its grid cursor supports CSI C/D movement across
wrapped lines; shell editing keeps command length separate from the insertion
position so moving left/right does not lose text. Up/down history still places
the insertion point at the end of the recalled command.

Run `make test-text-input test-notepad test-explorer` for host regression tests.
