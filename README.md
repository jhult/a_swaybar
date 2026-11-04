# swaybar.c

A status bar in one C file, 808 lines, speaking the i3bar JSON protocol to sway.

It shows volume, wifi, battery, CPU and the clock, and one coloured block per
maki session, ordered by workspace. It replaces the shell script it grew out of.

## The two decisions

**Volume is read on a change event, not on a timer.** The bar holds a
libpulse connection and asks for source info when PulseAudio says something
changed. At rest, pipewire-pulse does no work for the bar, and the bar does no
work for volume.

**The bar does not link against maki, and maki does not spawn the bar.** maki
writes one small file per session per process (see
[maki-session-status](https://github.com/zipadoodlez/maki-session-status)). The
bar resolves a pid to a workspace through sway, then reads the file. Either side
can be rewritten or restarted alone.

## Build and run

```sh
make                         # builds ./swaybar
```

Then point sway's `status_command` at the absolute path, with
`tray_output none` off and `bar { status_command /path/to/swaybar }`.

The binary must not be called `swaybar` on your `PATH`: sway spawns its own
renderer under that name and yours would hijack it. Keep an absolute path.

Requires: a C compiler, `libpulse` headers (`pkg-config libpulse`), and sway.

## What it reads

- `/proc` and `/sys` for CPU and battery.
- `inotify` for changes it should react to instead of polling.
- a Unix socket for the sway `i3-ipc` state, to map pid to workspace.
