# Xfburn-X

**Xfburn-X** (`xfburn-x`) is an **unofficial, AI-assisted fork** of Xfce's [Xfburn](https://gitlab.xfce.org/apps/xfburn) optical disc burning application (powered by [libburnia](https://dev.lovelyhq.com/libburnia/web/wiki) `libburn` / `libisofs`), adding multiple new features and numerous bugfixes.

`xfburn-x` uses separate binary, desktop, icon, translation, and configuration paths, so it can be installed in parallel alongside upstream `xfburn` without conflicts.

> [!IMPORTANT]
> **Pre-built `x86_64` (`amd64`) `.deb` packages for Ubuntu-based distributions** (Ubuntu 24.04 LTS, 26.04 LTS, Xubuntu, Linux Mint, Pop!_OS, etc.) are provided for download on the **[GitHub Releases](https://github.com/asdfjkl/xfburn-x/releases)** page.

## Installing on Ubuntu-Based Distributions (`.deb`)

1. Download the latest `xfburn-x_*_amd64.deb` from **[GitHub Releases](https://github.com/asdfjkl/xfburn-x/releases)**.
2. Install the `.deb` package and the required runtime libraries:

```bash
# 1. Install Xfburn-X
sudo apt install ./xfburn-x_*_amd64.deb

# 2. Uninstall Xfburn-X
sudo apt remove xfburn-x
```
---

## New Features

* **Verify after Burn** — Automatic Verification after burn: re-assesses the disc after burning, reads back every written sector via `libburn` with internal retries disabled, displays live verification progress/speed and error counts, and reports exact failing LBA ranges.
* **Disc Info Inspector (`Action → Disc Info`)** — Displays drive (drive model, firmware revision, serial number, etc.) and media metadata (i.e. Media ID, manufacturer etc.)
* **User-Mode Diagnostic Flight-Recorder Logging** — Automatically writes session logs to `~/.local/state/xfburn-x/logs/` (rotating the 10 most recent sessions; overridable via `--log-file=PATH` / `-l` or `XFBURN_X_LOG_FILE`). Captures real-time `libburn`/`libisofs` messages and SCSI sense errors, sysfs SCSI I/O counters, USB link speed/power/autosuspend and USB 3.0 LPM telemetry, udev/unmount events, call-duration and stall watchdogs, 2-second burn/verify progress heartbeats, and async-signal-safe crash stack traces, with a **"Save Log..."** button in the burn progress dialog. Attach these with error info if you run into issues
* **Drive Disconnect & Transport Error Diagnostics** — External BD-R drives (USB power/cables, USB 3.0 LPM `U1`/`U2` sleep states, runtime autosuspend) connected via USB 3.0 often run into power issues. Error diagnostic distinguishes physical USB/SATA drive disconnects and SCSI host/transport failures from genuine disc read/write errors directly
* **Asynchronous Non-Blocking Drive Probing & Unmounting** — Inserting discs, switching drives, or opening burn/blank/info dialogs no longer freezes the UI.
  
---

## Bugfixes (Since Forking from Upstream Xfburn)

* Fixed Blu-ray / Large Data Collection "Proceed to Burn" Grayed Out & Bogus Capacity Dropdown
* Fixed CD-RW `0x` Burn Speed Bug & Broken Media Profile Speed Mapping
* Fixed Missing / Unlabeled Write Speeds on Certain Drives & Media
* Fixed Erratic Burn & Verification Speed Readouts
* Fixed Post-Burn Udev / GDK Mutex Deadlocks & GUI Freezes
* Fixed Main-Thread Blocking Drive Grabs, `usleep()` Stalls & Udev Re-Probe Loops
* Fixed Concurrent Drive Grab / Refresh Races During Active Burns
* Fixed Fatal `g_error()` Crashes on Transient Drive Communication Errors
* Fixed Burn Image & Blank Dialog Race Conditions and Object Lifecycle Issues
  
---

## Requirements

* `libisofs` >= 0.6.2
* `libburn` >= 0.4.2 (0.5.6+ recommended)
* `gtk+-3.0` >= 3.24.0, `glib-2.0` >= 2.66.0
* `libxfce4util-1.0` >= 4.18.0, `libxfce4ui-2` >= 4.18.0, `exo-2` >= 4.18.0
* `gudev-1.0` >= 145 (recommended)
* `gstreamer-1.0` + `gstreamer-pbutils-1.0` + plugins (optional, for compressed Audio CD sources)
* `xsltproc` (for building the man page)

## Building, Testing & Installing from Source

Using the included `Makefile` wrapper (installs to `~/.local` by default):

```bash
make test      # configure, compile, and run unit tests
make install   # compile and install to ~/.local (updates icon/desktop caches)
make run       # build, install, and launch xfburn-x
make deb       # build and verify an amd64 .deb package in dist/
```

Or install system-wide with `sudo make install PREFIX=/usr/local`, or invoke Meson directly:

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
Xfburn-X is an unofficial fork of [Xfce Xfburn](https://gitlab.xfce.org/apps/xfburn) (Copyright 2005–2025 Xfce development team), with additions and maintenance Copyright 2026 Dominik Klein.
