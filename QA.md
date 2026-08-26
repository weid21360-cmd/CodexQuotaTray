# Verification checklist

## Automated

- Configure and build `x64 Release` with `BUILD_TESTING=ON`.
- Run `ctest -C Release --output-on-failure`.
- Confirm both `cqt_tests` and `cqt_client_tests` pass.
- Build the Inno Setup package and perform a clean per-user installation.

## Functional

- With ChatGPT sign-in, confirm plan, all quota windows, remaining percentages, reset times, and Token charts populate.
- With API-key auth, confirm the UI reports that ChatGPT sign-in is required without attempting login.
- Disconnect networking and confirm cached data remains visible with a delayed/stale badge.
- Kill the managed app-server and confirm reconnection backs off rather than spinning.
- Change the Codex executable path and confirm the child process restarts once.
- Confirm manual refresh is debounced for three seconds.

## Shell integration

- Restart Explorer and confirm the notification icon reappears.
- Test bottom, top, left, and right taskbars where supported.
- With auto-hide and full-screen presentation, confirm the capsule hides while the standard tray icon remains the fallback.
- Test 100%, 125%, and 150% scaling and multiple monitors.
- Confirm a second launch raises the existing window and does not start another app-server.

## Visual

- Compare Home, Settings, and More settings in dark/light mode with the supplied 436×650 references.
- Check Chinese and English for clipping at each supported window/font scale.
- Verify custom colors, glass fallback, progress bars, chart ranges, and unavailable states.

## Privacy and performance

- Inspect `%LocalAppData%\CodexQuotaTray`: only settings and aggregate cache data should exist.
- Search persisted data for `access_token`, `refresh_token`, `id_token`, prompt text, email, and raw JSON-RPC; none should occur.
- Hide the panel and run `scripts/measure-idle.ps1`; target average CPU is at most 0.1% and private memory at most 30 MB for the UI process.
- Run a 24-hour sample and verify no sustained growth in private memory, working set, handles, GDI objects, or USER objects.
- Measure the managed `codex app-server` process separately because its footprint is controlled by the installed Codex version.

