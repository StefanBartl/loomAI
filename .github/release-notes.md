## Download

**`loomai-setup-<version>-x64.exe`** - Windows installer (x64). It installs for the current user, no
administrator rights needed. The optional "add to PATH" step puts the `loomai` command into new terminals.
Start loomAI from the Start menu (or with `loomai` when you chose that step); it listens on `127.0.0.1:8080`.

**Or install it with the script of this release**, which downloads the installer itself, checks it against
`SHA256SUMS` and runs it silently for the current user. A file it downloads is not stopped by SmartScreen, and
nothing runs that does not match the checksum. In your own PowerShell, not elevated:

```
powershell -ExecutionPolicy Bypass -c "irm https://github.com/StefanBartl/loomAI/releases/download/{tag}/install.ps1 | iex"
```

That installs without the "add to PATH" step. With it (and any other argument of the script), use the form
that takes arguments:

```
& ([scriptblock]::Create((irm https://github.com/StefanBartl/loomAI/releases/download/{tag}/install.ps1))) -InstallArgs '/TASKS=addtopath'
```

Run the script again to repair this release (to update, take the script of a newer release). Stop loomAI
first if it is running; the setup tries to close it, but a clean stop is safer.

The installer is not code-signed. If you download it in the browser, Windows SmartScreen may show "Windows
protected your PC": choose *More info*, then *Run anyway*. To check a download first, compare
`Get-FileHash .\loomai-setup-<version>-x64.exe` with its line in `SHA256SUMS`. That proves the file is whole
and the one of this release, not who made the release.

Cloud backends need their API keys in the environment (`OPENAI_API_KEY`, `ANTHROPIC_API_KEY`,
`GEMINI_API_KEY`); the installer never stores secrets. See the README for the rest. Linux and macOS: build with
`scripts/setup.sh`.
