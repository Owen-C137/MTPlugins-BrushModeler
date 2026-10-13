param([string]$Python = "")
$ErrorActionPreference = "Stop"
$plugin = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($Python)) {
    $localPython = Join-Path $plugin "build\python\python.exe"
    $Python = if (Test-Path -LiteralPath $localPython) { $localPython } else { "python" }
}
$repo = $plugin
$monorepo = Join-Path $plugin "..\.."
if (Test-Path -LiteralPath (Join-Path $monorepo "RadiantPlugins.sln")) { $repo = (Resolve-Path $monorepo).Path }
$dist = Join-Path $plugin "build\dist"
$work = Join-Path $plugin "build\work"
$spec = Join-Path $plugin "build"
New-Item -ItemType Directory -Force -Path $dist,$work | Out-Null
Push-Location $repo
try {
    & $Python -m PyInstaller --noconfirm --clean --onefile --name merge_xmodels --console --distpath $dist --workpath $work --specpath $spec (Join-Path $plugin "merge_xmodels.py")
    if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed with exit code $LASTEXITCODE" }
} finally { Pop-Location }
$helper = Join-Path $dist "merge_xmodels.exe"
if (!(Test-Path -LiteralPath $helper)) { throw "PyInstaller did not create $helper" }
Copy-Item -LiteralPath $helper -Destination (Join-Path $plugin "merge_xmodels.exe") -Force
Write-Host "Built standalone helper: $plugin\merge_xmodels.exe"
