# Xfburn-X

**Xfburn-X** (`xfburn-x`) is an **unofficial, AI-assisted fork** of Xfce's [Xfburn](https://gitlab.xfce.org/apps/xfburn) optical disc burning application (powered by [libburnia](https://dev.lovelyhq.com/libburnia/web/wiki) `libburn` / `libisofs`), adding multiple new features and numerous bugfixes.

`xfburn-x` uses separate binary, desktop, icon, translation, and configuration paths, so it can be installed in parallel alongside upstream `xfburn` without conflicts.

> [!IMPORTANT]
> **Pre-built `x86_64` (`amd64`) `.deb` packages for Ubuntu-based distributions** (Ubuntu 24.04 LTS, 26.04 LTS, Xubuntu, Linux Mint, Pop!_OS, etc.) are provided for download on the **[GitHub Releases](https://github.com/asdfjkl/xfburn-x/releases)** page.

## Installing on Ubuntu-Based Distributions (`.deb`)

1. Download the latest `xfburn-x_*_amd64.deb` from **[GitHub Releases](https://github.com/asdfjkl/xfburn-x/releases)**.
2. Install the required runtime libraries and then install the `.deb` package:

```bash
# 1. Install required runtime libraries (and recommended GStreamer/GVfs packages)
sudo apt update
sudo apt install -y \
  hicolor-icon-theme libburn4t64 libexo-2-0 libgdk-pixbuf-2.0-0 \
  libglib2.0-0t64 libgstreamer-plugins-base1.0-0 libgstreamer1.0-0 \
  libgtk-3-0t64 libgudev-1.0-0 libisofs6t64 libxfce4ui-2-0 libxfce4util7 \
  gvfs gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-ugly gstreamer1.0-libav

# 2. Install Xfburn-X
sudo dpkg -i xfburn-x_*_amd64.deb
```

*(Alternatively, `sudo apt install ./xfburn-x_*_amd64.deb` installs the package and resolves all dependencies in one command. To uninstall later, run `sudo apt remove xfburn-x`.)*

---

## New Features

* **Automatic Post-Burn Per-Sector Verification ("Verify after burn")** — Enabled by default for ISO image burns, Data compositions (CD, DVD, Blu-ray, M-DISC), and Audio CDs (automatically disabled for Dummy write and ISO-only creation). Computes 64-bit per-sector hashes on the fly in RAM (~8 bytes/sector, no temporary disk space required), re-assesses the disc after burning, reads back every written sector via `libburn` with internal retries disabled, displays live verification progress/speed and error counts, and reports exact failing LBA ranges.
* **Disc Info Inspector (`Action → Disc Info`)** — Non-blocking modal dialog displaying detailed drive and media metadata via `libburn`: drive model, firmware revision, serial number, buffer size, and supported MMC profiles; disc status, media profile, manufacturer guess, Media ID (ASCII & Hex), Product ID, Book Type, DVD physical format/layers, CD ATIP Lead-In/Lead-Out MSF & LBA, BD spare area and BD-R POW; capacity, format descriptors, and Next Writable Address; supported write/read speeds and write modes; and a full Sessions & Tracks (TOC) table, with auto-refresh on media changes and one-click **Copy** to clipboard.
* **User-Mode Diagnostic Flight-Recorder Logging** — Automatically writes structured, unprivileged session logs to `~/.local/state/xfburn-x/logs/` (rotating the 10 most recent sessions; overridable via `--log-file=PATH` / `-l` or `XFBURN_X_LOG_FILE`). Captures real-time `libburn`/`libisofs` messages and SCSI sense errors, sysfs SCSI I/O counters, USB link speed/power/autosuspend and USB 3.0 LPM telemetry, udev/unmount events, call-duration and stall watchdogs, 2-second burn/verify progress heartbeats, and async-signal-safe crash stack traces, with a **"Save Log..."** button in the burn progress dialog.
* **Drive Disconnect & Transport Error Diagnostics** — Distinguishes physical USB/SATA drive disconnects and SCSI host/transport failures from genuine disc read/write errors directly via `libburn` messages and world-readable sysfs attributes, cleanly aborting affected burns or verifications with actionable hints (USB power/cables, USB 3.0 LPM `U1`/`U2` sleep states, runtime autosuspend, or SATA cabling) instead of misreporting a defective disc.
* **Asynchronous Non-Blocking Drive Probing & Unmounting** — Moves optical drive probing, speed/capability queries, and GIO unmount handling onto cancellable background worker threads so inserting discs, switching drives, or opening burn/blank/info dialogs no longer freezes the UI.
* **Updated Translations Across All 60 Languages** — Translated all newly introduced `xfburn-x` UI, dialog, diagnostic, desktop, and AppStream strings across every locale in `po/LINGUAS`.
* **Automated Unit Tests & Packaging Tooling** — Added Meson/GTest suites covering logging/error classification, burn speed calculations, async device probing, Disc Info reporting, and disc-usage widgets, along with a convenience `Makefile` wrapper, `debian/` packaging (`make deb`), and a GitHub Actions release pipeline.

---

## Bugfixes (Since Forking from Upstream Xfburn)

* **Fixed Blu-ray / Large Data Collection "Proceed to Burn" Grayed Out & Bogus Capacity Dropdown** — Fixed a GObject initialization bug in `XfburnDiscUsage` where populating the disc-size dropdown inside `_init()` read the base class's 7 placeholder test entries (`"size 1"` … `"10^12"`) instead of the concrete Data or Audio subclass media tables. This capped data compositions at 7.9 GB DVD and left **Proceed to Burn** disabled for Blu-ray collections (e.g. 20 GB on a 22.5 GB BD-R). Moved dropdown population to `constructed()`, fixed auto-selection ordering, and added bounds guards.
* **Fixed CD-RW `0x` Burn Speed Bug & Broken Media Profile Speed Mapping** — Fixed an off-by-one check in `xfburn_media_profile_to_kb()` where CD-RW (`0x0a`) failed `media_no <= XFBURN_PROFILE_CDR (0x09)`, fell back to Blu-ray 1x speed (`4496 kB/s`), and truncated 10x CD-RW (`1764 kB/s`) to `0x`. Replaced range heuristics with an explicit switch covering all CD, DVD (including DVD-ROM, DVD-R DL Jump, DVD+RW DL), and BD (including BD-ROM and BD-R RRM) profiles with exact floating-point 1x factors.
* **Fixed Missing / Unlabeled Write Speeds on Certain Drives & Media** — Added CD ATIP probing (`burn_disc_read_atip()`) and `libburn` min/max write speed fallback discovery when `burn_drive_get_speedlist()` is incomplete, formatted fractional speeds accurately as `<x>x (<kB/s> kB/s)` (e.g. `2.4x` DVD+RW), and replaced raw `-1` fallback labels with `"Min"`.
* **Fixed Erratic Burn & Verification Speed Readouts** — Replaced noisy instantaneous/cumulative speed calculations with windowed moving-average speed readouts during both writing and verification, and surfaced live verification read/verify error counts directly in the progress dialog status line.
* **Fixed Runaway Fake Read-Error Cascade on Drive Disconnect During Verification** — Fixed verification continuing into single-sector fallback probing after a drive disconnected mid-verify, which previously caused the read-error counter to climb rapidly as if the disc were unreadable. Verification now retries once after a short delay on transient SCSI host resets while the drive remains grabbed, and immediately aborts with a dedicated drive-disconnect communication warning/error when connection is lost.
* **Fixed Post-Burn Udev / GDK Mutex Deadlocks & GUI Freezes** — Resolved GDK thread-lock deadlocks and UI freezes triggered when udev volume-change events or background blank/unmount threads interacted with GTK main-thread callbacks.
* **Fixed Main-Thread Blocking Drive Grabs, `usleep()` Stalls & Udev Re-Probe Loops** — Removed blocking synchronous drive grabs and `usleep()` polling from the GTK main thread, added synthetic udev release cooldown suppression so releasing a probed drive does not immediately re-trigger endless refreshes, and added clean probe drain synchronization before `burn_finish()` on application exit.
* **Fixed Concurrent Drive Grab / Refresh Races During Active Burns** — Serialized drive acquisition so background device refreshes and Disc Info scans yield immediately whenever an active burn or blank operation holds the drive.
* **Fixed Fatal `g_error()` Crashes on Transient Drive Communication Errors** — Replaced fatal `g_error()` aborts on transient `libburn` drive errors with graceful error reporting and diagnostic logging.
* **Fixed Burn Image & Blank Dialog Race Conditions and Object Lifecycle Issues** — Removed redundant synchronous media grabs in the ISO Burn Image dialog by routing updates through the async device box, disabled blanking while a device refresh is in progress, guarded against `NULL` device selection, added proper `XfburnDevice` reference counting in the blanking worker thread, and corrected `XfburnUdevManagerClass` to inherit from `GObjectClass` instead of `XfburnProgressDialogClass`.
* **Fixed Desktop & Application Icon Resolution Issues** — Installed `stock_xfburn-x` icons under `hicolor/*/apps/` in addition to `stock/media/`, added `StartupWMClass=xfburn-x` to `xfburn-x.desktop`, declared `hicolor-icon-theme` as a `.deb` dependency, and updated `make uninstall` to refresh user-local icon and desktop caches so stale local caches do not shadow system `.deb` installs.

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
