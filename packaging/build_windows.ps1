param([switch]$Console)
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$extension = Get-ChildItem (Join-Path $repoRoot "build\bindings") -Filter "_nexus*.pyd" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $extension) {
    throw "The _nexus Python extension is missing. Build the project first with CMake."
}

$windowMode = if ($Console) { "--console" } else { "--windowed" }
$windowsIcu = Join-Path $env:WINDIR "System32\icuuc.dll"
if (-not (Test-Path -LiteralPath $windowsIcu)) {
    throw "Windows ICU (icuuc.dll) was not found at $windowsIcu"
}
python -m PyInstaller `
    --noconfirm `
    --clean `
    --onefile `
    $windowMode `
    --name "NEXUS Search" `
    --paths (Join-Path $repoRoot "gui") `
    --paths (Split-Path -Parent $extension.FullName) `
    --distpath $repoRoot `
    --workpath (Join-Path $repoRoot "build\pyinstaller") `
    --runtime-hook (Join-Path $repoRoot "packaging\pyi_rth_nexus.py") `
    --add-binary "$windowsIcu;." `
    --hidden-import _nexus `
    (Join-Path $repoRoot "gui\qt_main.py")

if ($LASTEXITCODE -ne 0) {
    throw "PyInstaller failed with exit code $LASTEXITCODE"
}
# The build environment's PATH can expose Poppler's suffixed ICU build. The
# Windows ICU DLL above is embedded at the bundle root for Qt's import table.
Write-Host "Packaged application: $(Join-Path $repoRoot 'NEXUS Search.exe')"
