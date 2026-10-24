**This is not a release.** It is the newest state of `main`, rebuilt after every green CI run and replaced each time. Nothing here is signed or tested by hand; use a release for anything that matters. Before it is offered, the installer is installed silently on a clean runner (through the install script below), started, asked `GET /health`, installed again over the running server and uninstalled; after it is offered, the published script is run once more from here on a second runner.

**Install it with the script** (Windows; for the current user, no administrator rights, and no SmartScreen stop: the script downloads the installer itself and checks it against `SHA256SUMS`). In your own PowerShell, not elevated:

```
powershell -ExecutionPolicy Bypass -c "irm https://github.com/StefanBartl/loomAI/releases/download/latest/install.ps1 | iex"
```

That installs without the "add to PATH" step. With it (and any other argument of the script), use the form that takes arguments:

```
& ([scriptblock]::Create((irm https://github.com/StefanBartl/loomAI/releases/download/latest/install.ps1))) -InstallArgs '/TASKS=addtopath'
```

Run it again to update to the newest rolling build (stop loomAI first if it is running). Or take the file yourself:

- `loomai-setup-x64.exe`, the Windows installer under a fixed name. The version inside is `0.0.<number of the run>`, which is what *Settings -> Apps* shows, also when this build is newer than a release. Windows SmartScreen warns about an unsigned installer you downloaded in the browser: "More info", then "Run anyway".
- `SHA256SUMS` holds the checksums of both files.

There is no Linux or macOS file: build there with `scripts/setup.sh`. Start loomAI from the Start menu (or with `loomai` when "add to PATH" was chosen); it listens on `127.0.0.1:8080`. Cloud backends need their API keys in the environment (`OPENAI_API_KEY`, `ANTHROPIC_API_KEY`, `GEMINI_API_KEY`); the installer never stores secrets.
