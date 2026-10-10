# Puts a downloaded, already checked Corded update in place on Windows.
#
# Windows will not replace a program while it runs, so the app starts this
# script, hidden, and ends. The script waits for the app to be gone, copies the
# new files over the old ones, and starts the app again. Everything it does is
# written to the log, which the app reads on its next start if this failed.
param(
    [Parameter(Mandatory = $true)][int]$ProcessId,     # the app that is about to end
    [Parameter(Mandatory = $true)][string]$Source,     # the unpacked update
    [Parameter(Mandatory = $true)][string]$Target,     # the folder the app lives in
    [Parameter(Mandatory = $true)][string]$Start,      # the program to start afterwards
    [Parameter(Mandatory = $true)][string]$Log
)

function Note([string]$line) {
    Add-Content -LiteralPath $Log -Value ("{0}  {1}" -f (Get-Date -Format 'HH:mm:ss'), $line) -Encoding UTF8
}

Set-Content -LiteralPath $Log -Value "Corded update: $Source -> $Target" -Encoding UTF8
try {
    Note "waiting for the app ($ProcessId) to close"
    try { Wait-Process -Id $ProcessId -Timeout 60 -ErrorAction Stop } catch { }

    # The program's files can stay locked for a moment after it ends; try a few times.
    $copied = $false
    for ($try = 1; $try -le 20 -and -not $copied; $try++) {
        try {
            Copy-Item -Path (Join-Path $Source '*') -Destination $Target -Recurse -Force -ErrorAction Stop
            $copied = $true
        } catch {
            Note "copy attempt $try did not go through: $($_.Exception.Message)"
            Start-Sleep -Milliseconds 500
        }
    }
    if (-not $copied) { throw 'the new files could not be copied over the old ones' }
    Note 'copied'
    Start-Process -FilePath $Start -WorkingDirectory $Target
    Note 'started'
    Note 'OK'
} catch {
    Note "FAILED: $($_.Exception.Message)"
    # Leave the person with a working app: the old one is still there.
    try { Start-Process -FilePath $Start -WorkingDirectory $Target } catch { }
    exit 1
}
