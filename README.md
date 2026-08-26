# CodexQuotaTray

A lightweight native Windows tray monitor for Codex quota and token activity. The UI follows the supplied compact dark-panel references while remaining DPI-aware and usable in light mode.

## Features

- Reads ChatGPT Codex quota through the documented local `codex app-server --stdio` JSON-RPC interface.
- Shows returned short-term, weekly, monthly, or unknown-duration quota windows without assuming that `primary` always means five hours.
- Displays remaining percentage, reset countdown, reset time, account plan, and service health.
- Provides 24-hour local Token activity plus 7-day and 30-day account activity with local fallback.
- Includes a standard notification-area icon and an optional non-invasive taskbar capsule.
- Supports Chinese, English, system/dark/light appearance, four palettes, custom colors, sizing controls, startup toggle, cached offline display, and manual Codex executable selection.
- Does not read or persist authentication tokens. See [PRIVACY.md](PRIVACY.md).

## Runtime requirements

- Windows 10 22H2 or Windows 11, x64.
- Codex Desktop or Codex CLI signed in with a ChatGPT account.
- A `codex.exe` that supports `app-server`, `account/rateLimits/read`, and `account/usage/read`. If it is not on `PATH`, select it from **More settings → Codex CLI**.

API-key-only sessions do not expose ChatGPT plan quota and are intentionally reported as requiring ChatGPT sign-in.

## Build

The repository contains source only. Building requires a Visual Studio 2022 C++ desktop toolchain, a Windows 10/11 SDK, and CMake 3.21 or newer. No compiler is downloaded by the project scripts.

```powershell
./scripts/build.ps1 -Configuration Release
```

To create the per-user installer when Inno Setup 6 is installed:

```powershell
./scripts/build.ps1 -Configuration Release -Package
```

The installer output is written to `dist/`. It does not require administrator rights and leaves “Start with Windows” unchecked by default.

## Architecture

- `CodexClient`: managed stdio child process, JSONL framing, request correlation, timeouts, and sparse update notifications.
- `usage_parser`: quota normalization and account Token bucket parsing.
- `UsageHistory`: low-priority, incremental parsing of only local `token_count` metadata.
- `SettingsStore`: atomic settings and snapshot cache in `%LocalAppData%\CodexQuotaTray`.
- `MainWindow`: Win32 + Direct2D/DirectWrite three-page UI with per-monitor DPI behavior.
- `TrayIcon`: `Shell_NotifyIcon`, dynamic percentage icon, context menu, and optional taskbar-adjacent capsule.

The UI process performs no hidden-window animation. Quota refreshes once per minute, Token data refreshes every five minutes, and connection failures back off to five minutes.

## Tests

`cqt_tests` covers JSON parsing, current and legacy quota shapes, percentage clamping, duration classification, taskbar selection, ChatGPT/API-key account handling, and daily usage buckets. `tests/fake_app_server.ps1` is a deterministic manual integration fixture.

See [QA.md](QA.md) for the complete Windows/DPI/Explorer/privacy checklist. After launching the app, `scripts/measure-idle.ps1` measures CPU, private memory, working set, and handle growth without installing any tools.
