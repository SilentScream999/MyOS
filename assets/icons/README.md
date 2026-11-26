# Wyzar desktop icons

These PNG files are ordinary runtime resources, installed under `/share/icons`.
They are original geometric artwork for this project. The optional
`tools/create_default_icons.py` authoring script documents their shapes; it is
never run by the build or required for application/theme installation.

Explorer, Notepad, Task Manager and Music Player use large folder, spiral-notebook,
CPU-monitor and shaded record/play silhouettes with transparent backgrounds. Their artwork fills most of each icon canvas instead
of sitting inside a padded colored tile. Regenerate these families with
`python3 tools/create_default_icons.py --only explorer`, `--only notepad`, or
`--only task_manager`, or `--only music_player`.

An application can ship its own PNG and refer to it by path in its manifest.
See `docs/IMAGES.md` for icon names, themes, runtime updates and image limits.
