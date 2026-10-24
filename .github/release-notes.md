## Download

**`loomai-setup-<version>-x64.exe`** - Windows installer (x64). It installs for the current user, no
administrator rights needed. The optional "add to PATH" step adds the `loomai` command to new terminals. Start
loomAI from the Start menu or with `loomai`; it listens on `127.0.0.1:8080`.

**Or install it with the script of this release**, which downloads the installer itself, checks it against
`SHA256SUMS` and runs it silently for the current user. A file it downloads is not stopped by SmartScreen, and
nothing runs that does not match the checksum. In your own PowerShell, not elevated:

```
powershell -ExecutionPolicy Bypass -c "irm https://github.com/StefanBartl/loomAI/releases/download/{tag}/install.ps1 | iex"
```

(While this is a draft, the files cannot be fetched by that line: a draft's files download only for people who
may write to the repository, logged in. They work once the release is published. To try the script on a draft,
download the installer, `SHA256SUMS` and `install.ps1` into one folder and run
`.\install.ps1 -File .\loomai-setup-<version>-x64.exe`.) Saved as a file, the script takes more:
`-InstallArgs '/TASKS=addtopath'` puts `loomai` on your `PATH`.

The installer is not code-signed. If you download it in the browser, Windows SmartScreen may show "Windows
protected your PC": choose *More info*, then *Run anyway*. To check a download first, compare
`Get-FileHash .\loomai-setup-<version>-x64.exe` with its line in `SHA256SUMS`. That proves the file is whole
and the one of this release, not who made the release.

Cloud backends need their API keys in the environment (`OPENAI_API_KEY`, `ANTHROPIC_API_KEY`,
`GEMINI_API_KEY`); the installer never stores secrets. See the README for the rest. Linux and macOS: build with
`scripts/setup.sh`.
