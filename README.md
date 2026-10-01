# Claude Usage Monitor

A dependency-free Visual Studio 2022 C11 command-line monitor for Claude Code on Windows.

It continuously scans Claude Code's local JSONL transcripts and displays:
- latest Claude Code session
- cumulative input/output/cache tokens for that session
- latest context-window usage
- request count
- a graphical context bar
- optional live 5-hour / 7-day rate limits when Claude Code status-line data is bridged to the monitor

## Build

Open `ClaudeUsageMonitor.sln` in Visual Studio 2022 and build `Release | x64`.

Or from a VS Developer Command Prompt:

    msbuild ClaudeUsageMonitor.sln /p:Configuration=Release /p:Platform=x64

Run:

    ClaudeUsageMonitor.exe

Press Ctrl+C to quit.

## Important limitation

Claude Code's live subscription rate-limit percentages are supplied to a configured status-line command at runtime; they are not reliably available in the transcript files. The monitor therefore gets token/session information directly from local transcripts and can optionally receive the live status-line JSON through its `/bridge` mode.

To use the live rate-limit display, configure Claude Code's `statusLine` command to invoke:

    C:\path\to\ClaudeUsageMonitor.exe /bridge

The bridge consumes the JSON Claude Code sends to a status-line command and saves it under `%LOCALAPPDATA%\ClaudeUsageMonitor\status.json`.

If you already have a custom status line, keep it and have it also invoke the bridge; do not overwrite an existing status line blindly.

## What the numbers mean

"Session tokens" are summed from Claude Code transcript usage records for the most recently active session. "Context" is the latest context-window usage reported by the most recent assistant response.

The 5-hour and 7-day percentages, when present, come from Claude Code's own `rate_limits` status-line payload.
