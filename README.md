# Xfburn-X

**Xfburn-X** (`xfburn-x`) is an actively enhanced fork of Xfce's [Xfburn](https://gitlab.xfce.org/apps/xfburn) optical disc burning application, powered by the [libburnia](https://dev.lovelyhq.com/libburnia/web/wiki) (`libburn` / `libisofs`) libraries.

Xfburn-X is designed for reliable archival burning (including CD, DVD, Blu-ray, and M-DISC BD-R media) and **installs cleanly side-by-side with upstream `xfburn`** with zero filename, desktop, icon, translation, or configuration conflicts.

## What's New in Xfburn-X (vs. Upstream Xfburn)

### 1. Automatic Post-Burn Per-Sector Verification ("Verify after burn")
* Enabled by default on ISO image burns, Data compositions (CD / DVD / BD), and Audio CD compositions (automatically disabled for Dummy write and ISO-only creation).
* Hashes every sector on the fly as data flows to the burner (64-bit FNV-1a + Murmur3 finalizer, ~8 bytes/sector in RAM, no temporary disk space required).
* Re-assesses the disc after writing and reads back all written sectors via `libburn` (`burn_read_data` / `burn_read_audio`) with internal retries disabled, reporting exact failing LBA ranges if any read error or hash mismatch occurs.

### 2. User-Mode Diagnostic Flight-Recorder Logging
* Automatically writes a structured, single-line-per-event diagnostic log to `~/.local/state/xfburn-x/logs/xfburn-x-YYYYMMDD-HHMMSS-<pid>.log` (keeping the 10 most recent sessions), overridable via `--log-file=PATH` (`-l`) or `XFBURN_X_LOG_FILE`.
* Runs 100% in standard unprivileged user mode (no root permissions required).
* Captures:
  * Real-time `libburn` and `libisofs` messages (all severities, error codes, OS `errno`, and SCSI sense errors).
  * Kernel SCSI device state and I/O counters (`iorequest_cnt`, `iodone_cnt`, `ioerr_cnt`), USB link speed (`speed_mbps`), bus power budget (`bMaxPower`), and runtime power management (`power/control`, `power/runtime_status`) from `/sys`.
  * Udev `block`, `scsi`, and `usb` hotplug/disconnect events and GIO unmount timings.
  * Call-duration tracing, loop stall watchdogs, 2-second burn/verify progress heartbeats, and async-signal-safe crash stack traces (`SIGSEGV`/`SIGABRT`/`SIGBUS`/`SIGFPE`).
* Includes a **"Save Log..."** button directly in the burn progress dialog for attaching logs to bug reports.

### 3. Threading, GDK Mutex & Drive Reliability Fixes
* Fixes GDK thread lock corruption and UI freezes triggered by udev callbacks and background unmount loops.
* Prevents concurrent drive grab/refresh races during active burns/verifications and eliminates blocking `usleep()` calls on the GTK main thread.
* Replaces fatal `g_error()` aborts on transient drive communication errors with graceful error handling.

## Side-by-Side Co-Installation with Upstream `xfburn`

Xfburn-X uses completely isolated names and paths so both applications can be installed on the same machine simultaneously:

| Component | Upstream `xfburn` | `xfburn-x` |
|---|---|---|
| Binary | `xfburn` | `xfburn-x` |
| Desktop launcher | `xfburn.desktop` (`Xfburn`) | `xfburn-x.desktop` (`Xfburn-X`) |
| AppStream ID | `org.xfce.xfburn` | `io.github.asdfjkl.xfburn-x` |
| UI data directory | `<datadir>/xfburn/` | `<datadir>/xfburn-x/` |
| Hicolor icons | `stock_xfburn*` | `stock_xfburn-x*` |
| Translations | `xfburn.mo` | `xfburn-x.mo` |
| Man page | `xfburn(1)` | `xfburn-x(1)` |
| User settings | `~/.config/xfburn/settings.xml` | `~/.config/xfburn-x/settings.xml` |
| Diagnostic logs | *(n/a)* | `~/.local/state/xfburn-x/logs/` |

## Requirements

* `libisofs` >= 0.6.2
* `libburn` >= 0.4.2 (0.5.6+ recommended)
* `gtk+-3.0` >= 3.24.0, `glib-2.0` >= 2.66.0
* `libxfce4util-1.0` >= 4.18.0, `libxfce4ui-2` >= 4.18.0
* `gudev-1.0` >= 145 (recommended)
* `gstreamer-1.0` + `gstreamer-pbutils-1.0` + plugins (optional, for compressed Audio CD sources)
* `xsltproc` (for building the man page)

## Building, Testing & Installing

```bash
meson setup build --prefix="$HOME/.local"
meson compile -C build
meson test -C build --verbose
meson install -C build
```

Run `xfburn-x` from your application menu (**Xfburn-X**) or terminal:

```bash
xfburn-x
# Or with a custom diagnostic log path:
xfburn-x --log-file=/tmp/xfburn-x-debug.log
```

## Bugs & Feedback

Please report bugs and attach your diagnostic log file (saved via **"Save Log..."** or from `~/.local/state/xfburn-x/logs/`) at:
[https://github.com/asdfjkl/xfburn-x/issues](https://github.com/asdfjkl/xfburn-x/issues)

## License & Attribution

Xfburn-X is released under the **GNU General Public License v2.0 or later** (`GPL-2.0-or-later`). See [COPYING](COPYING) for the full license text.
Xfburn-X is forked from [Xfce Xfburn](https://gitlab.xfce.org/apps/xfburn) (Copyright 2005–2025 Xfce development team), with additions and maintenance Copyright 2026 Dominik Klein.
