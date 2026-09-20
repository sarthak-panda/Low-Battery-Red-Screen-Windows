# Low Battery Red Screen (Windows)

[![CI](https://github.com/sarthak-panda/Low-Battery-Red-Screen-Windows/actions/workflows/ci.yml/badge.svg)](https://github.com/sarthak-panda/Low-Battery-Red-Screen-Windows/actions/workflows/ci.yml)

A tiny, always-on Windows utility that **turns your whole screen red when the battery is low and the laptop is not charging** - so you notice *before* the machine dies.

- 🔴 Full-screen red tint across **all monitors** (translucent, so you can still see your work)
- 🖱️ **Zero interference** - click-through, never takes focus, no taskbar / Alt-Tab entry
- 🔁 **Self-healing** - starts at every sign-in and restarts itself if it crashes or freezes
- 🪶 **Tiny** - ~34 KB exe, no dependencies, no admin rights, no network access

---

## Install (end users)

1. Open the [**Releases**](../../releases/latest) page and download **`LowBatteryRed-v*-win64.zip`** (or just `LowBatteryRed.exe`).
2. Extract the zip and **double-click `LowBatteryRed.exe`** (or `Install.bat`). That's it - a confirmation box appears and the app is running.
3. If Windows **SmartScreen** shows *"Windows protected your PC"*: click **More info → Run anyway**.
   The exe is not code-signed (that costs money), which is the only reason for the warning. You can read every line of the source here, or [build it yourself](#build-from-source).
   Some antivirus tools are wary of unsigned programs that register autostart; if yours flags it, build from source or allow it.
4. **Try it:** double-click `Test.bat` → the screen turns red for 6 seconds.

**Requirements:** Windows 10 / 11, 64-bit (ARM64 PCs run it through Windows' x64 emulation).

### Uninstall

Double-click `Uninstall.bat` (from the zip), or run:

```
%LOCALAPPDATA%\LowBatteryRed\LowBatteryRed.exe --uninstall
```

This stops the app and removes everything listed below.

### What installation changes on your PC

| What | Where |
|---|---|
| Program + settings | `%LOCALAPPDATA%\LowBatteryRed\` (`LowBatteryRed.exe`, `config.ini`) |
| Autostart | `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` → value `LowBatteryRed` |
| Safety-net task | Task Scheduler task `LowBatteryRed` (current user; at sign-in + every minute; battery restrictions **disabled**) |

Nothing else: no services, no drivers, no admin/UAC prompt, no network connections.

---

## How it behaves

| Situation | Screen |
|---|---|
| On battery, at or below threshold (default **20 %**) | 🔴 red |
| Battery critical (< 5 %) and not charging | 🔴 red |
| Charger connected / battery charging | normal (red disappears immediately) |
| Just above the threshold | normal (2 % hysteresis prevents flicker at the boundary) |
| Desktop PC with no battery, or battery status unknown | normal (never shows) |

### Configuration

Edit `%LOCALAPPDATA%\LowBatteryRed\config.ini`, save - changes apply within ~2 seconds, **no restart needed**.

```ini
[Settings]
Threshold=20   ; battery % at or below which the screen goes red (1-99)
Opacity=55     ; red tint strength 10-100  (100 = solid red, no see-through)
```

Invalid or out-of-range values fall back to the defaults / are clamped.

### Quick real-world check

Set `Threshold=99`, unplug the charger → screen goes red. Plug in → it clears. Set it back to `20`.

### Command line

| Command | Effect |
|---|---|
| `LowBatteryRed.exe` | install (copy to `%LOCALAPPDATA%`, enable autostart) and start |
| `LowBatteryRed.exe --test` | show the red screen for 6 s, then exit |
| `LowBatteryRed.exe --uninstall` | stop everything and remove it |
| `--silent` (add to any of the above) | no message boxes |
| `--run` / `--worker <pid>` | internal (supervisor / overlay process) |

To check it is running: Task Manager → Details → two `LowBatteryRed.exe` processes (supervisor + worker).

---

## How it works

```
 sign-in ──► Run key / Scheduled Task ──► LowBatteryRed.exe --run   (supervisor, ~no code to crash)
                                              │  spawns / restarts / watches
                                              ▼
                                   LowBatteryRed.exe --worker      (overlay + battery polling)
                                              ▲
                     supervisor dies ─────────┘ worker relaunches it
```

- **Overlay:** one borderless window spanning the entire virtual desktop with the styles `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST`, translucent via `SetLayeredWindowAttributes`. It answers `WM_NCHITTEST` with `HTTRANSPARENT` and `WM_MOUSEACTIVATE` with `MA_NOACTIVATE`, and is only ever shown with `SWP_NOACTIVATE`, so it cannot take focus or swallow input. It is re-asserted topmost every 2 s so the taskbar can't cover it.
- **Battery:** `GetSystemPowerStatus` polled every 2 s, plus immediate reaction to `WM_POWERBROADCAST` (plug/unplug, resume from sleep) and `WM_DISPLAYCHANGE` (monitor changes).
- **Crash recovery:** the worker installs an unhandled-exception filter that exits instantly (no "stopped working" dialog to block the restart). The supervisor restarts it with back-off, and kills + restarts it if its window stops responding for ~30 s. Single-instance mutexes prevent duplicates.
- **Autostart:** HKCU Run key (always works, no admin) plus a per-user Scheduled Task as a backstop. The task explicitly turns off "don't start on battery" / "stop on battery", which would otherwise defeat the purpose.
- **Per-monitor DPI aware** via the embedded manifest, so the overlay is pixel-exact on mixed-DPI multi-monitor setups.

## Limitations

- Games/videos in true **exclusive fullscreen** can draw above any overlay (borderless/windowed fullscreen is fine).
- Windows' lock screen and UAC prompts are separate secure desktops and are not tinted.
- Unsigned binary → SmartScreen warning on first run (see above).

---

## Build from source

The project is a single C file (`src/lowbatteryred.c`) built with MinGW-w64.

**Linux / macOS / WSL (cross-compile):**
```sh
sudo apt install gcc-mingw-w64-x86-64 zip     # Debian/Ubuntu
./build.sh                                     # -> build/LowBatteryRed.exe
./package.sh 1.0.0                             # -> build/release/ (exe, zip, SHA256SUMS.txt)
```

**Windows (MSYS2 MinGW64 shell):**
```sh
pacman -S --needed mingw-w64-x86_64-gcc zip
./build.sh
```
`build.sh` honours `CC` and `WINDRES` if your toolchain uses different names.

The exe is built with `-Wl,--no-insert-timestamp`, so the same source built with the same toolchain gives a byte-identical exe (CI checks this on every run; a different MinGW version may produce different bytes).

## Tests

`tests/run_tests.sh` runs the **real Windows binary under Wine** with a virtual display and drives it from a Windows test program (`tests/test_harness.c`).

```sh
sudo apt install gcc-mingw-w64-x86-64 wine64 xvfb
./tests/run_tests.sh
```

It uses a *test build* (`-DLBR_TEST`) that differs from the release build only in two ways: the battery status is read from a file (`LBR_SIM_FILE`) so scenarios can be simulated, and timers are faster. It checks:

- overlay covers the full virtual screen exactly; window styles; opacity; click-through hit-testing
- **focus is never stolen** (foreground/active/focus window unchanged, with a positive control proving the check can detect a steal)
- battery logic: unplugged, charging, threshold edges, hysteresis, critical flag, no battery, unknown status
- crash recovery, killing the supervisor, hang detection, no duplicate processes
- install / upgrade / uninstall, Run-key registration, live `config.ini` reload, invalid config values
- the generated Task Scheduler XML (captured via a `schtasks.exe` test double), including the fallback when a logon trigger is rejected

**What the Wine tests can't cover:** real battery hardware and real Windows DWM compositing (translucency). Use the [quick real-world check](#quick-real-world-check) after installing.

## CI / CD (GitHub Actions)

| Workflow | Runs on | What it does |
|---|---|---|
| [`ci.yml`](.github/workflows/ci.yml) | every push to `main` and every pull request | shellcheck → reproducible-build check → full Wine test suite (`-Werror`) → packaging dry-run. Uploads the test log, the CI-built exe and a run summary. |
| `ci.yml` › *Native Windows run* | after the job above | **Experimental, non-blocking.** Runs the same harness on a real `windows-latest` runner and verifies the real Task Scheduler round-trip (task created, battery restrictions off, removed on uninstall). |
| [`release.yml`](.github/workflows/release.yml) | pushing a tag `vX.Y.Z` | checks the tag is on `main`, re-runs the full suite as a gate, builds, and publishes the GitHub Release with `LowBatteryRed.exe`, the zip and `SHA256SUMS.txt`. |

**Cutting a release**

```sh
git checkout main && git pull
git tag v1.0.1
git push origin v1.0.1      # CI tests, packages and publishes the release
```

Tags containing a `-` (e.g. `v1.1.0-rc1`) are published as pre-releases.


## Repository layout

```
src/         lowbatteryred.c, app.manifest, app.rc
tests/       test_harness.c, fake_schtasks.c, run_tests.sh
.github/workflows/   ci.yml, release.yml
dist/        Install.bat, Test.bat, Uninstall.bat, README.txt  (shipped inside the release zip)
build.sh     build the exe            package.sh   create release assets
```

## Sample Outcome

<img width="2878" height="1798" alt="image" src="https://github.com/user-attachments/assets/27e90948-6f24-4ebd-9044-5329a59e8952" />

## Thanks for Visiting the Repo...

<img width="736" height="414" alt="image" src="https://github.com/user-attachments/assets/638b033b-a608-4d46-9605-d51a3b35273f" />


