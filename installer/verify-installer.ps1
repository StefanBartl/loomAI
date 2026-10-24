<#
.SYNOPSIS
    Installs a built loomai-setup-*.exe silently, checks the installed copy and
    uninstalls it again.

.DESCRIPTION
    Used by .github/workflows/release.yml and rolling.yml (through the action
    .github/actions/installer-build) so that a release is only offered
    after the installer itself has been proven: files in place, user PATH entry
    added (and its other entries untouched), the launcher starts the server,
    GET /health answers with a minimal PATH (so no MinGW on PATH can hide a
    missing DLL), and the uninstaller removes the files and puts the user PATH
    entries back as they were.

    This really installs and edits the user PATH of whoever runs it, so it
    refuses to run outside CI unless -Force is given.

.PARAMETER Setup
    Path to loomai-setup-<version>-x64.exe.

.PARAMETER WorkDir
    Scratch directory the installer installs into. Default: $env:RUNNER_TEMP\verify-install.

.PARAMETER InstallScript
    Path of the install.ps1 of appkit's release-kit. With it the setup is installed the way a user installs a
    release: through that script (-File, -Sha256 of the setup, -Kind inno, -AllowElevated because a runner is
    elevated), which runs it silently. Without it the setup is started directly.

.PARAMETER Force
    Run outside CI.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Setup,
    [string]$WorkDir = (Join-Path $env:RUNNER_TEMP 'verify-install'),
    [string]$InstallScript = '',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

if (-not $Force -and $env:CI -ne 'true') {
    throw 'Refusing to run outside CI: this installs the setup for real and edits the user PATH. Pass -Force to override.'
}

$Setup = (Resolve-Path -LiteralPath $Setup).Path
$InstallDir = Join-Path $WorkDir 'loomAI'
$SetupLog = Join-Path $WorkDir 'setup.log'
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$Port = 8080

function Write-Ok([string]$Text) { Write-Host "  [ OK ] $Text" -ForegroundColor Green }

function Assert-That([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "verification failed: $Message" }
}

# Raw (unexpanded) user PATH, '' if there is none -- the same view the installer has.
function Get-RawUserPath {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment')
    if (-not $key) { return '' }
    try {
        return [string]$key.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    } finally {
        $key.Close()
    }
}

function Get-NormalizedUserPath {
    $entries = @((Get-RawUserPath) -split ';' | Where-Object { $_ })
    return $entries -join ';'
}

function Wait-ForHealth {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 250
        try {
            $health = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 2
            if ($health.status -eq 'ready') { return $health }
        } catch { }
    }
    return $null
}

function Stop-InstalledServer {
    $exe = Join-Path $InstallDir 'app\loomai.exe'
    Get-Process loomai -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -ieq $exe } |
        Stop-Process -Force
}

# One installation of the setup into $InstallDir: through the install script of the release-kit when there is
# one, else by starting the setup. It throws when the installation fails.
function Install-Setup {
    if ($InstallScript) {
        $kitScript = (Resolve-Path -LiteralPath $InstallScript).Path
        Write-Host "`n==> Installing $(Split-Path -Leaf $Setup) through $kitScript to $InstallDir"
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Setup).Hash
        # The script checks the hash, runs the setup silently for the current user and throws when it fails;
        # the extra arguments are the ones this check needs (the folder, the PATH task, a log). It is called
        # in this session, not through `pwsh -File`: a command line cannot carry an array, and the three
        # arguments would arrive as one string.
        & $kitScript -File $Setup -Sha256 $hash -Kind inno -AllowElevated `
            -InstallArgs @('/TASKS=addtopath', "/DIR=$InstallDir", "/LOG=$SetupLog")
        Write-Ok 'the install script finished'
    } else {
        Write-Host "`n==> Installing $(Split-Path -Leaf $Setup) silently to $InstallDir"
        $install = Start-Process -FilePath $Setup -Wait -PassThru -ArgumentList @(
            '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-',
            "/DIR=`"$InstallDir`"", '/TASKS=addtopath', "/LOG=`"$SetupLog`""
        )
        Assert-That ($install.ExitCode -eq 0) "installer exited with code $($install.ExitCode)"
        Write-Ok 'installer exited with 0'
    }
}

function Assert-InstalledFiles {
    foreach ($rel in @('app\loomai.exe', 'app\dashboard\index.html', 'loomai.cmd', 'unins000.exe')) {
        Assert-That (Test-Path (Join-Path $InstallDir $rel)) "missing $rel"
    }
    Assert-That (@(Get-ChildItem (Join-Path $InstallDir 'app') -Filter *.dll).Count -gt 0) 'no runtime DLLs next to loomai.exe'
    Write-Ok 'binary, dashboard, runtime DLLs, launcher and uninstaller are in place'
}

# The server through the launcher, as a user starts it: a minimal PATH (no MinGW on it that could hide a
# missing DLL) and a foreign working directory.
function Start-InstalledServer {
    $savedPath = $env:Path
    $env:Path = "$env:SystemRoot\System32;$env:SystemRoot"
    try {
        $null = Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" -WindowStyle Hidden `
            -WorkingDirectory $env:TEMP -ArgumentList '/c', "`"$(Join-Path $InstallDir 'loomai.cmd')`""
    } finally {
        $env:Path = $savedPath
    }
    $health = Wait-ForHealth
    Assert-That ($null -ne $health) 'the installed server did not answer /health within 20 s'
    Write-Ok "/health: status=$($health.status)"
    $dashboard = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$Port/" -TimeoutSec 5
    Assert-That ($dashboard.StatusCode -eq 200) "dashboard answered $($dashboard.StatusCode)"
    Write-Ok 'dashboard served (the launcher started the server from the right directory)'
}

function Get-InstalledServer {
    $exe = Join-Path $InstallDir 'app\loomai.exe'
    return @(Get-Process loomai -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq $exe })
}

$pathBefore = Get-RawUserPath
$entriesBefore = Get-NormalizedUserPath
$installed = $false
try {
    Install-Setup
    $installed = $true

    Write-Host "`n==> Checking the installed files"
    Assert-InstalledFiles

    Write-Host "`n==> Checking the user PATH (task addtopath)"
    $pathInstalled = Get-RawUserPath
    $entries = @($pathInstalled -split ';' | Where-Object { $_ })
    Assert-That (@($entries | Where-Object { $_.TrimEnd('\') -ieq $InstallDir.TrimEnd('\') }).Count -eq 1) 'install directory is not on the user PATH exactly once'
    if ($pathBefore) {
        Assert-That ($pathInstalled.StartsWith($pathBefore)) 'the installer changed existing user PATH entries'
    }
    Write-Ok 'install directory added, existing entries untouched'

    Write-Host "`n==> Starting the server through the launcher (minimal PATH, foreign working directory)"
    Start-InstalledServer

    # "Run it again to update" is what the scripts tell a user, and the server is a console process that keeps
    # its files open: the setup must not fail on it, and what is installed afterwards must be whole and start.
    # Whether the setup closes the running server is told, not required (a user can stop it).
    Write-Host "`n==> Installing again over the running server (an update)"
    Assert-That ((Get-InstalledServer).Count -gt 0) 'the server is not running before the update'
    Install-Setup
    Start-Sleep -Seconds 2
    if ((Get-InstalledServer).Count -gt 0) {
        Write-Host '  [INFO] the old server is still running after the update (the setup did not close it)'
    } else {
        Write-Host '  [INFO] the setup closed the running server'
    }
    Stop-InstalledServer
    Assert-InstalledFiles
    Write-Host "`n==> Starting the updated copy"
    Start-InstalledServer
} catch {
    if (Test-Path $SetupLog) {
        Write-Host '--- installer log (tail) ---'
        Get-Content $SetupLog -Tail 25
    }
    throw
} finally {
    Stop-InstalledServer
}

if ($installed) {
    Write-Host "`n==> Uninstalling"
    $uninstaller = Join-Path $InstallDir 'unins000.exe'
    # The uninstaller copies itself to %TEMP% and exits at once, so -Wait does
    # not wait for the real work: poll for the result instead.
    # No server may hold a file of the install: it was stopped above, and a killed process lets go of its
    # files a moment later.
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ((Get-InstalledServer).Count -gt 0 -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 250 }
    $UninstallLog = Join-Path $WorkDir 'uninstall.log'
    $null = Start-Process -FilePath $uninstaller -Wait -PassThru -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$UninstallLog`""
    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    while ((Test-Path (Join-Path $InstallDir 'app')) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 500 }
    if (Test-Path (Join-Path $InstallDir 'app')) {
        # What is left, and what the uninstaller says about it.
        Write-Host '--- left in the install directory ---'
        Get-ChildItem -LiteralPath $InstallDir -Recurse -Force | ForEach-Object { $_.FullName }
        Write-Host '--- loomai processes ---'
        Get-Process loomai -ErrorAction SilentlyContinue | ForEach-Object { "$($_.Id) $($_.Path)" }
        if (Test-Path $UninstallLog) {
            Write-Host '--- uninstaller log (tail) ---'
            Get-Content $UninstallLog -Tail 40
        }
    }
    Assert-That (-not (Test-Path (Join-Path $InstallDir 'app'))) 'the uninstaller left the app directory behind'
    Assert-That (-not (Test-Path (Join-Path $InstallDir 'loomai.cmd'))) 'the uninstaller left the launcher behind'
    Write-Ok 'files removed'
    # Compare entry lists, not raw text: removing our entry may drop an empty
    # trailing ';' that was there before, which changes nothing for Windows.
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ((Get-NormalizedUserPath) -cne $entriesBefore -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 500 }
    Assert-That ((Get-NormalizedUserPath) -ceq $entriesBefore) 'the user PATH entries were not restored'
    Write-Ok 'user PATH entries restored (every other entry verbatim, %VAR% references included)'
}

Write-Host "`nInstaller verified." -ForegroundColor Green
