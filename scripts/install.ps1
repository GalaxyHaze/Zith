param(
    [string]$Version = "",
    [switch]$Lsp
)

$Repo = if ([string]::IsNullOrWhiteSpace($env:ZITH_REPOSITORY)) {
    "GalaxyHaze/Zith-Lang"
} else {
    $env:ZITH_REPOSITORY
}
$InstallRootOverride = $env:ZITH_INSTALL_ROOT
$ReleaseBaseUrl = $env:ZITH_RELEASE_BASE_URL

if ([string]::IsNullOrWhiteSpace($Version)) {
    Write-Host "No version specified. Fetching latest version..."
    try {
        $Response = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest"
        $Version = $Response.tag_name
        Write-Host "Latest version found: $Version" -ForegroundColor Green
    } catch {
        Write-Error "Failed to fetch latest version from GitHub. Check your internet connection."
        exit 1
    }
} else {
    Write-Host "Installing requested version: $Version" -ForegroundColor Yellow
}

if (-not $Version.StartsWith("v")) {
    $Version = "v$Version"
}

if ([string]::IsNullOrWhiteSpace($ReleaseBaseUrl)) {
    $ReleaseBaseUrl = "https://github.com/$Repo/releases/download/$Version"
} else {
    $ReleaseBaseUrl = $ReleaseBaseUrl.TrimEnd("/")
}

# Detect OS and architecture
$FileName = ""
$LspFileName = ""
$IsArm64 = $false

# Check if we're on an ARM64 Windows
$Arch = (Get-CimInstance -Class Win32_Processor | Select-Object -First 1).Caption
if ($Arch -match "ARM" -or $env:PROCESSOR_ARCHITECTURE -match "ARM64") {
    $IsArm64 = $true
}

if ($IsArm64) {
    $FileName = "zithc-windows-arm64.exe"
    $LspFileName = "zith-lsp-windows-arm64.exe"
} else {
    $FileName = "zithc-windows-amd64.exe"
    $LspFileName = "zith-lsp-windows-amd64.exe"
}

$DownloadUrl = "$ReleaseBaseUrl/$FileName"
$TempPath = "$env:TEMP\zithc-installer.exe"
$LspTempPath = "$env:TEMP\zith-lsp-installer.exe"
$StdlibArchivePath = "$env:TEMP\zithc-stdlib.zip"
$DownloadDir = Join-Path $env:TEMP "zithc-release-$PID"
$GitHubCLI = Get-Command gh -ErrorAction SilentlyContinue
$UseGitHubCLI = -not [string]::IsNullOrWhiteSpace($env:GITHUB_TOKEN) -and
    [string]::IsNullOrWhiteSpace($env:ZITH_RELEASE_BASE_URL) -and $null -ne $GitHubCLI
$ZithRoot = if ([string]::IsNullOrWhiteSpace($InstallRootOverride)) {
    Join-Path $env:LOCALAPPDATA "Zith"
} else {
    $InstallRootOverride
}
$InstallDir = Join-Path $ZithRoot "bin"
$StdlibDir = Join-Path $ZithRoot "share\zith\stdlib"

Write-Host "Downloading from $DownloadUrl..." -ForegroundColor Cyan
$FailedAsset = $FileName

try {
    if ($UseGitHubCLI) {
        New-Item -ItemType Directory -Path $DownloadDir -Force | Out-Null
        $PreviousGhToken = $env:GH_TOKEN
        $env:GH_TOKEN = $env:GITHUB_TOKEN
        try {
            & $GitHubCLI.Source release download $Version `
                --repo $Repo `
                --pattern $FileName `
                --pattern "zithc-stdlib-$Version.zip" `
                --dir $DownloadDir
            if ($LASTEXITCODE -ne 0) {
                throw "GitHub CLI failed to download release assets"
            }
            if ($Lsp) {
                $FailedAsset = $LspFileName
                & $GitHubCLI.Source release download $Version `
                    --repo $Repo `
                    --pattern $LspFileName `
                    --dir $DownloadDir
                if ($LASTEXITCODE -ne 0) {
                    throw "GitHub CLI failed to download the language server asset"
                }
            }
        } finally {
            if ($null -eq $PreviousGhToken) {
                Remove-Item Env:GH_TOKEN -ErrorAction SilentlyContinue
            } else {
                $env:GH_TOKEN = $PreviousGhToken
            }
        }
        $FailedAsset = $FileName
        Copy-Item -Path (Join-Path $DownloadDir $FileName) -Destination $TempPath -Force
        $FailedAsset = "zithc-stdlib-$Version.zip"
        Copy-Item -Path (Join-Path $DownloadDir "zithc-stdlib-$Version.zip") `
            -Destination $StdlibArchivePath -Force
        if ($Lsp) {
            $FailedAsset = $LspFileName
            Copy-Item -Path (Join-Path $DownloadDir $LspFileName) `
                -Destination $LspTempPath -Force
        }
    } else {
        $FailedAsset = $FileName
        Invoke-WebRequest -Uri $DownloadUrl -OutFile $TempPath -UseBasicParsing
        if ($Lsp) {
            $FailedAsset = $LspFileName
            Invoke-WebRequest -Uri "$ReleaseBaseUrl/$LspFileName" `
                -OutFile $LspTempPath -UseBasicParsing
        }
    }
} catch {
    $DownloadError = $_
    $FailedUrl = "$ReleaseBaseUrl/$FailedAsset"
    Write-Error "Failed to download required asset '$FailedAsset' for release '$Version' from $FailedUrl. GitHub reported: $($DownloadError.Exception.Message)"
    exit 1
}

Write-Host "Installing Zith to $InstallDir..."

try {
    # Stage and validate the stdlib before changing the installed compiler.
    $StdlibStage = Join-Path $env:TEMP "zithc-stdlib-stage-$PID"
    $StdlibUrl = "$ReleaseBaseUrl/zithc-stdlib-$Version.zip"
    Write-Host "Downloading stdlib..." -ForegroundColor Cyan
    if (-not $UseGitHubCLI) {
        Invoke-WebRequest -Uri $StdlibUrl -OutFile $StdlibArchivePath -UseBasicParsing
    }
    if (Test-Path $StdlibStage) {
        Remove-Item -Path $StdlibStage -Recurse -Force
    }
    New-Item -ItemType Directory -Path $StdlibStage -Force | Out-Null
    Expand-Archive -Path $StdlibArchivePath -DestinationPath $StdlibStage -Force
    if (-not (Test-Path (Join-Path $StdlibStage "std\io\console.zithm"))) {
        throw "downloaded stdlib is missing std\io\console.zithm"
    }

    if (-not (Test-Path $InstallDir)) {
        New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
    }

    Copy-Item -Path $TempPath -Destination "$InstallDir\zithc.exe" -Force
    Remove-Item -Path $TempPath -Force
    if ($Lsp) {
        Copy-Item -Path $LspTempPath -Destination "$InstallDir\zith-lsp.exe" -Force
        Remove-Item -Path $LspTempPath -Force
    }

    $UserPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $InstallPathEntry = $InstallDir.TrimEnd('\')
    if (($UserPath -split ';') -notcontains $InstallPathEntry) {
        if ([string]::IsNullOrWhiteSpace($UserPath)) {
            $NewUserPath = $InstallPathEntry
        } else {
            $NewUserPath = $UserPath.TrimEnd(';') + ';' + $InstallPathEntry
        }
        [Environment]::SetEnvironmentVariable('Path', $NewUserPath, 'User')
        Write-Host "Added $InstallDir to your user PATH." -ForegroundColor Green
    }

    if (Test-Path $StdlibDir) {
        Remove-Item -Path $StdlibDir -Recurse -Force
    }
    $StdlibParent = Split-Path -Parent $StdlibDir
    New-Item -ItemType Directory -Path $StdlibParent -Force | Out-Null
    Move-Item -Path $StdlibStage -Destination $StdlibDir -Force
    Remove-Item -Path $StdlibArchivePath -Force
    if ($UseGitHubCLI) {
        Remove-Item -Path $DownloadDir -Recurse -Force
    }
    Write-Host "Standard library installed to $StdlibDir" -ForegroundColor Green

    Write-Host "--------------------------------------------------"
    Write-Host "Installation Complete!" -ForegroundColor Green
    Write-Host "Run 'zithc --help' in a NEW terminal window to get started."
    Write-Host "--------------------------------------------------"
} catch {
    Write-Error "Failed to install to $InstallDir."
    Write-Host "You might need to run PowerShell as Administrator, or manually move the file from:"
    Write-Host "$TempPath"
    Write-Host "to a folder in your PATH."
    exit 1
}
