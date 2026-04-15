param(
    [string]$Version = ""
)

# 1. Setup Global Variables
$Repo = "GalaxyHaze/Zith"
$ApiUrl = "https://api.github.com/repos/$Repo/releases/latest"

# 2. Determine Version
if ( [string]::IsNullOrWhiteSpace($Version))
{
    Write-Host "No version specified. Fetching latest version..."
    try
    {
        $Response = Invoke-RestMethod -Uri $ApiUrl
        $Version = $Response.tag_name
        Write-Host "Latest version found: $Version" -ForegroundColor Green
    }
    catch
    {
        Write-Error "Failed to fetch latest version from GitHub. Check your internet connection."
        exit 1
    }
}
else
{
    Write-Host "Installing requested version: $Version" -ForegroundColor Yellow
}

# 3. Detect OS and Architecture
# Windows is usually AMD64, but we check just in case
$FileName = ""
$IsWindow = $true

if ($IsWindow)
{
    $FileName = "zith-windows-amd64.exe"
}
else
{
    # Fallback (shouldn't happen on PS, but good practice)
    Write-Error "Unsupported Operating System."
    exit 1
}

# 4. Setup Download URL and Destination
$DownloadUrl = "https://github.com/$Repo/releases/download/$Version/$FileName"
# We download to the temp folder first to avoid permission issues
$TempPath = "$env:TEMP\zith-installer.exe"
# Final destination (e.g. C:\Users\You\AppData\Local\Microsoft\WindowsApps)
# Note: Sometimes WindowsApps is write-protected. ProgramData is often safer for tools,
# but we'll try CurrentUser AppData first.
$InstallDir = "$env:LOCALAPPDATA\Microsoft\WindowsApps"

# 5. Download the binary
Write-Host "Downloading from $DownloadUrl..." -ForegroundColor Cyan

try
{
    # -UseBasicParsing is essential to avoid errors in server environments
    Invoke-WebRequest -Uri $DownloadUrl -OutFile $TempPath -UseBasicParsing
}
catch
{
    Write-Error "Failed to download file. The version '$Version' might not exist."
    exit 1
}

# 6. Install
Write-Host "Installing Zith to $InstallDir..."

try
{
    # Ensure the directory exists
    if (-not (Test-Path $InstallDir))
    {
        New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
    }

    # Move the file
    Move-Item -Path $TempPath -Destination "$InstallDir\zith.exe" -Force

    Write-Host "--------------------------------------------------"
    Write-Host "Installation Complete!" -ForegroundColor Green
    Write-Host "Run 'zith --help' in a NEW terminal window to get started."
    Write-Host "--------------------------------------------------"
}
catch
{
    Write-Error "Failed to move file to $InstallDir."
    Write-Host "You might need to run PowerShell as Administrator, or manually move the file from:"
    Write-Host "$TempPath"
    Write-Host "to a folder in your PATH."
    exit 1
}