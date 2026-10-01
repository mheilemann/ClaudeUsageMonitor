# Claude Usage Monitor

A dependency-free Visual Studio 2022 C11 command-line monitor for Claude Code on Windows.

It continuously scans Claude Code's local JSONL transcripts and displays:
- latest Claude Code session
- cumulative input/output/cache tokens for that session
- latest context-window usage
- request count
- a graphical context bar
- live 5-hour / 7-day subscription usage and reset times

## Build

Open `ClaudeUsageMonitor.sln` in Visual Studio 2022 and build `Release | x64`.

Or from a VS Developer Command Prompt:

    msbuild ClaudeUsageMonitor.sln /p:Configuration=Release /p:Platform=x64

Run:

    ClaudeUsageMonitor.exe

Press Ctrl+C to quit.

## Live 5-hour / 7-day limits

While the monitor is running, a background thread polls the same endpoint Claude Code's `/usage` command uses (`https://api.anthropic.com/api/oauth/usage`) every 30 seconds. It authenticates with Claude Code's own login token from `%USERPROFILE%\.claude\.credentials.json`; the token is re-read on every poll (Claude Code rotates it) and is only ever sent to `api.anthropic.com`. The result is written atomically to `%LOCALAPPDATA%\ClaudeUsageMonitor\status.json`.

This works however Claude Code is run, including headless hosts such as IDE extensions. If the token has expired, run Claude Code once to refresh it.

### Status-line bridge (terminal CLI)

The monitor also points Claude Code's `statusLine` (in `%USERPROFILE%\.claude\settings.json`) at itself, so interactive terminal sessions push fresh data after every response:

    "statusLine": {
      "type": "command",
      "command": "C:/path/to/CLAUDE~1.EXE --bridge",
      "refreshInterval": 5
    }

The command uses forward slashes, an unquoted 8.3 short path and `--bridge` (not `/bridge`), because Claude Code runs status-line commands through Git Bash (or PowerShell), and Git Bash strips backslashes and rewrites `/bridge` into a file path. A backup of the previous file is saved as `settings.json.bak`. An existing status line belonging to something else is left alone. Status lines only run in the interactive terminal UI.

## What the numbers mean

"Session tokens" are summed from Claude Code transcript usage records for the most recently active session. "Context" is the latest context-window usage reported by the most recent assistant response.

The 5-hour and 7-day percentages and reset times are the same figures Claude Code's `/usage` command shows.
