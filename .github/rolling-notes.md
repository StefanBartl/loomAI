**This is not a release.** It is the newest state of `main`, rebuilt after every green CI run and replaced each time. Nothing here is signed or tested by hand; use a release for anything that matters. Before it is offered, the installer is installed silently on a clean runner, started, asked `GET /health` and uninstalled.

**Install it with the script** (Windows; for the current user, no administrator rights, and no SmartScreen stop: the script downloads the installer itself and checks it against `SHA256SUMS`). In your own PowerShell, not elevated:

```
powershell -ExecutionPolicy Bypass -c "irm https://github.com/StefanBartl/loomAI/releases/download/latest/install.ps1 | iex"
```

Run it again to update to the newest rolling build. Saved as a file, the script takes more: `-InstallArgs '/TASKS=addtopath'` puts the `loomai` command on your `PATH`. Or take the file yourself:

- `loomai-setup-x64.exe`, the Windows installer under a fixed name (the version inside is `0.0.<run number>`). Windows SmartScreen warns about an unsigned installer you downloaded in the browser: "More info", then "Run anyway".
- `SHA256SUMS` holds the checksums of both files.

There is no Linux or macOS file: build there with `scripts/setup.sh`. Start loomAI from the Start menu or with `loomai`; it listens on `127.0.0.1:8080`. Cloud backends need their API keys in the environment (`OPENAI_API_KEY`, `ANTHROPIC_API_KEY`, `GEMINI_API_KEY`); the installer never stores secrets.
