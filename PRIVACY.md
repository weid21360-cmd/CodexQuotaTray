# Privacy design

CodexQuotaTray is a local, read-only quota display.

- It starts the documented `codex app-server` process and asks that process for account type, quota windows, and token-activity summaries.
- It never opens or parses token values in `~/.codex/auth.json` and never stores access, ID, or refresh tokens.
- For the 24-hour local chart, it considers only JSONL records whose exact shape is `event_msg` with `payload.type = token_count`. Prompt text, assistant output, tool arguments, and tool output are ignored.
- Persistent data contains settings, quota percentages, reset timestamps, aggregate token counts, and the last successful refresh time only.
- Diagnostic errors are status messages only. Raw JSON-RPC payloads, email addresses, prompts, and credentials are not logged.
- The application has no independent analytics, telemetry, advertising, or update service.

Runtime data is stored under `%LocalAppData%\CodexQuotaTray` and can be removed by the user at any time.

