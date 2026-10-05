# Put an "ASCIIArt" entry in the Start menu, so the app can be launched by
# name instead of from a terminal or a folder.
#
#   powershell -ExecutionPolicy Bypass -File install-shortcut.ps1
#
# Pass -Remove to delete it again.

param([switch]$Remove)

$here = Split-Path -Parent $MyInvocation.MyCommand.Definition
$exe = Join-Path $here 'asciiapp.exe'
$programs = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
$link = Join-Path $programs 'ASCIIArt.lnk'

if ($Remove) {
    if (Test-Path $link) { Remove-Item $link; "Removed $link" } else { "Nothing to remove." }
    return
}

if (-not (Test-Path $exe)) {
    Write-Error "asciiapp.exe not found next to this script - run build.bat first."
    exit 1
}

$shell = New-Object -ComObject WScript.Shell
$sc = $shell.CreateShortcut($link)
$sc.TargetPath = $exe
$sc.WorkingDirectory = $here
$sc.Description = 'View images and play video as ASCII art'
$sc.IconLocation = "$exe,0"
$sc.Save()

"Created $link"
"Press Start and type ASCIIArt."
