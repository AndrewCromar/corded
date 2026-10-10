# Downloads the newest Corded release for Windows and starts the terminal client.
#
# Run it from PowerShell with one line (everything after the last parenthesis
# is passed to the client):
#
#   & ([scriptblock]::Create((irm https://raw.githubusercontent.com/AndrewCromar/corded/main/packaging/install.ps1))) --server HOST:7443 --name yourname
#
# Files go to %LOCALAPPDATA%\Corded. Nothing is installed system-wide and no
# administrator rights are needed. Run the same line again later to update.
#
# Corded is pre-alpha and has not been security audited.
param(
    [switch]$NoRun,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ClientArgs
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$headers = @{ 'User-Agent' = 'corded-installer' }

# The newest release that has a Windows build (pre-releases included).
$releases = Invoke-RestMethod -Headers $headers -Uri 'https://api.github.com/repos/AndrewCromar/corded/releases?per_page=10'
$asset = $null
$tag = $null
foreach ($release in $releases) {
    $candidate = $release.assets | Where-Object { $_.name -like '*-windows-x64.zip' } | Select-Object -First 1
    if ($candidate) { $asset = $candidate; $tag = $release.tag_name; break }
}
if (-not $asset) { throw 'No Corded release with a Windows build was found.' }

$root = Join-Path $env:LOCALAPPDATA 'Corded'
$target = Join-Path $root $tag
$client = Get-ChildItem -Path $target -Filter 'corded-tui.exe' -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1

if (-not $client) {
    Write-Host "Downloading Corded $tag ..."
    New-Item -ItemType Directory -Force $target | Out-Null
    $zip = Join-Path $target 'corded.zip'
    Invoke-WebRequest -Headers $headers -Uri $asset.browser_download_url -OutFile $zip -UseBasicParsing
    Expand-Archive -Path $zip -DestinationPath $target -Force
    Remove-Item $zip
    $client = Get-ChildItem -Path $target -Filter 'corded-tui.exe' -Recurse | Select-Object -First 1
    if (-not $client) { throw 'The download did not contain corded-tui.exe.' }
} else {
    Write-Host "Corded $tag is already downloaded."
}
Write-Host "Corded $tag is in $($client.DirectoryName)"

if ($NoRun) { return }

# Keep the vault outside the versioned folder so updates do not lose it.
$allArgs = @()
if ($ClientArgs) { $allArgs += $ClientArgs }
if (-not ($allArgs -contains '--vault')) { $allArgs = @('--vault', (Join-Path $root 'vault')) + $allArgs }
& $client.FullName @allArgs
