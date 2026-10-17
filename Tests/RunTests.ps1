# Builds and runs the BrushModeler tests:
#  1. unit checks of the geometry, GDT and XMODEL_EXPORT code;
#  2. byte-for-byte comparison of the C++ merge with the reference Python helper (Tests\reference\merge_xmodels.py,
#     the helper shipped up to 0.3.14) on real models exported by the bundled ExportX;
#  3. export2bin compiling the C++ results (brushes, merges and a mirror).
param(
    [string]$Bo3Root = "D:\SteamLibrary\steamapps\common\Call of Duty Black Ops III",
    [string]$Python = "python",
    [int]$ModelCount = 8,
    [string]$Work = (Join-Path $env:TEMP "BrushModelerTests")
)
# Native tools write progress to stderr, which Windows PowerShell turns into errors under "Stop"; exit codes are checked instead.
$ErrorActionPreference = "Continue"
$tests = Split-Path -Parent $MyInvocation.MyCommand.Path
$plugin = Split-Path -Parent $tests
if (Test-Path -LiteralPath $Work) { Remove-Item -LiteralPath $Work -Recurse -Force }
$src = Join-Path $Work "src"
New-Item -ItemType Directory -Force -Path $Work, $src | Out-Null

# ---- build -------------------------------------------------------------------------------------------------------
$installer = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer"
$env:PATH = "$installer;$env:PATH"   # vcvars64.bat runs vswhere from PATH
$vswhere = Join-Path $installer "vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw "Visual Studio with the C++ tools was not found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$exe = Join-Path $Work "BrushModelerTests.exe"
$sources = @("Tests\BrushModelerTests.cpp", "Geometry.cpp", "XModelExport.cpp", "Gdt.cpp") | ForEach-Object { "`"$(Join-Path $plugin $_)`"" }
cmd /c "`"$vcvars`" >nul && cl /nologo /std:c++17 /O2 /EHsc /W3 /DNOMINMAX /Fe`"$exe`" /Fo`"$Work\\`" $($sources -join ' ')"
if ($LASTEXITCODE -ne 0) { throw "Test build failed." }

# ---- unit checks -------------------------------------------------------------------------------------------------
& $exe
if ($LASTEXITCODE -ne 0) { throw "Unit checks failed." }

# ---- reference comparison ----------------------------------------------------------------------------------------
$failures = 0
function Compare-Exports([string]$name, [string]$expected, [string]$actual) {
    $a = Get-Content -LiteralPath $expected -Raw; $b = Get-Content -LiteralPath $actual -Raw
    if ($a -ceq $b) { Write-Host "PASS $name"; return }
    $script:failures++
    $la = $a -split "`n"; $lb = $b -split "`n"
    for ($i = 0; $i -lt [Math]::Max($la.Count, $lb.Count); $i++) {
        if ($la[$i] -cne $lb[$i]) { Write-Host "FAIL $name at line $($i + 1): python='$($la[$i])' c++='$($lb[$i])'"; break }
    }
}
$reference = Join-Path $Work "merge_xmodels.py"   # run from the work folder: it logs beside itself
Copy-Item -LiteralPath (Join-Path $tests "reference\merge_xmodels.py") -Destination $reference
$exportx = Join-Path $plugin "Tools\exportx.exe"

# A spread of model sizes from model_export, exported to text the way the plugin does.
$bins = Get-ChildItem -LiteralPath (Join-Path $Bo3Root "model_export") -Recurse -File -Filter *.xmodel_bin -ErrorAction SilentlyContinue |
    Where-Object { $_.Length -gt 2000 -and $_.Length -lt 3MB -and $_.FullName -notmatch '_brush_modeler' } | Sort-Object Length
if ($bins.Count -lt 2) { throw "Need at least two .xmodel_bin files under model_export for the merge comparison." }
$step = [Math]::Max(1, [Math]::Floor($bins.Count / $ModelCount))
$picked = @(); for ($i = 0; $i -lt $bins.Count -and $picked.Count -lt $ModelCount; $i += $step) { $picked += $bins[$i] }
$models = @()
for ($i = 0; $i -lt $picked.Count; $i++) {
    $name = "m$i"
    Copy-Item -LiteralPath $picked[$i].FullName -Destination (Join-Path $src "$name.xmodel_bin")
    Push-Location $src; try { & $exportx -f "$name.xmodel_bin" -m export | Out-Null } finally { Pop-Location }
    $exp = Join-Path $src "$name.xmodel_export"
    if ((Test-Path -LiteralPath $exp) -and (Get-Item -LiteralPath $exp).Length -gt 0) { $models += [pscustomobject]@{ Name = $picked[$i].Name; Export = $exp } }
    else { Write-Host "skip $($picked[$i].Name): ExportX produced no export" }
}
Write-Host "models exported: $($models.Count)"

$placements = @(
    @{ origin = @("12345.678", "-2048.5", "96.25"); angles = @("0", "37.5", "0") },
    @{ origin = @("-130000.125", "7.75", "-512"); angles = @("10", "200", "-5") },
    @{ origin = @("0", "0", "0"); angles = @("0", "0", "0") },
    @{ origin = @("64.333", "128.666", "-1.001"); angles = @("-45", "90", "180") }
)
$center = @("100.125", "-50.5", "8")
function Write-Manifests([string]$stem, $list) {
    $json = @(); $text = @()
    for ($i = 0; $i -lt $list.Count; $i++) {
        $p = $placements[$i % $placements.Count]
        $json += "{`"export`":`"$($list[$i].Export.Replace('\','/'))`",`"origin`":[$($p.origin -join ',')],`"center`":[$($center -join ',')],`"angles`":[$($p.angles -join ',')]}"
        $text += "$($list[$i].Export) $($p.origin -join ' ') $($center -join ' ') $($p.angles -join ' ')"
    }
    Set-Content -LiteralPath "$stem.json" -Value ("[" + ($json -join ",") + "]") -Encoding ascii
    Set-Content -LiteralPath "$stem.txt" -Value $text -Encoding ascii
}
function Invoke-Reference([string[]]$arguments) {
    & $Python -I $reference @arguments
    if ($LASTEXITCODE -ne 0) { throw "Reference helper failed: $arguments" }
}

$brush = Join-Path $Work "brushes.xmodel_export"
& $exe brush-export $brush
$empty = Join-Path $Work "empty.xmodel_export"
& $exe empty-export $empty

# Brushes only (the old --optimize-only path).
Copy-Item $brush "$Work\py_opt.xmodel_export"
Invoke-Reference @("--optimize-only", "$Work\py_opt.xmodel_export")
& $exe optimize $brush "$Work\cpp_opt.xmodel_export"
Compare-Exports "brushes only" "$Work\py_opt.xmodel_export" "$Work\cpp_opt.xmodel_export"

# Brushes + every model, models only, and each model alone (exercises every strategy the models need).
$cases = @(@{ Name = "brushes+all models"; Base = $brush; Models = $models; Empty = $false },
           @{ Name = "models only"; Base = $empty; Models = $models; Empty = $true })
for ($i = 0; $i -lt $models.Count; $i++) { $cases += @{ Name = "brushes+$($models[$i].Name)"; Base = $brush; Models = @($models[$i]); Empty = $false } }
$n = 0
foreach ($case in $cases) {
    $n++
    $stem = Join-Path $Work "case$n"
    Write-Manifests $stem $case.Models
    Copy-Item $case.Base "$stem.py.xmodel_export"
    $extra = @()   # assigned separately: an if-expression would unroll the one-item array into a string
    if ($case.Empty) { $extra = @("--allow-empty-base") }
    Invoke-Reference (@("--merge-many", "$stem.py.xmodel_export", "$stem.json") + $extra)
    & $exe merge-many $case.Base "$stem.txt" "$stem.cpp.xmodel_export" @extra | Out-Host
    Compare-Exports $case.Name "$stem.py.xmodel_export" "$stem.cpp.xmodel_export"
}

# ---- export2bin accepts what the C++ code writes -----------------------------------------------------------------
$export2bin = Join-Path $Bo3Root "bin\export2bin.exe"
$compile = Join-Path $Work "compile"
New-Item -ItemType Directory -Force -Path $compile | Out-Null
Copy-Item "$Work\cpp_opt.xmodel_export" "$compile\bm_brushes.xmodel_export"
Copy-Item "$Work\case1.cpp.xmodel_export" "$compile\bm_merged.xmodel_export"
& $exe mirror $models[0].Export 1 "$compile\bm_mirror_x.xmodel_export" | Out-Host
& $exe mirror $models[[Math]::Min(1, $models.Count - 1)].Export 7 "$compile\bm_mirror_xyz.xmodel_export" | Out-Host
foreach ($file in Get-ChildItem $compile -Filter *.xmodel_export) {
    Push-Location $compile; try { & $export2bin /v $file.Name *> "$($file.FullName).log" } finally { Pop-Location }
    $bin = [IO.Path]::ChangeExtension($file.FullName, ".xmodel_bin")
    if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $bin)) { Write-Host "PASS export2bin $($file.Name)" }
    else { $failures++; Write-Host "FAIL export2bin $($file.Name) (exit $LASTEXITCODE, see $($file.FullName).log)" }
}

if ($failures) { throw "$failures comparison(s) failed. Work folder: $Work" }
Write-Host "All BrushModeler tests passed. Work folder: $Work"
