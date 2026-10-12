param(
    [string]$Version = "0.3.10",
    [string]$Bo3Root = "D:\SteamLibrary\steamapps\common\Call of Duty Black Ops III",
    [string]$Python = "python"
)
$ErrorActionPreference = "Stop"
if ($Version -notmatch '^\d+(\.\d+)*$') { throw "Version must contain only numbers and dots." }
$plugin = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $plugin "..\..")
$distRoot = Join-Path $repo "dist"
$out = Join-Path $distRoot "BrushModeler-$Version"
$candidate = Join-Path $out "BrushModeler"
$indexFiles = Join-Path $out "index-files\plugins\BrushModeler\$Version"
$msbuild = @(
    "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (!$msbuild) { throw "MSBuild not found in the known Visual Studio locations." }

& (Join-Path $plugin "build-merge-helper.ps1") -Python $Python
if ($LASTEXITCODE -ne 0) { throw "Merge helper build failed." }
& $msbuild (Join-Path $plugin "BrushModeler.vcxproj") /t:Build /p:Configuration=Release /p:Platform=x64 /p:DeployToGame=false "/p:Bo3Root=$Bo3Root" /v:m /nologo
if ($LASTEXITCODE -ne 0) { throw "Brush Modeler DLL build failed." }

if (Test-Path -LiteralPath $out) {
    $resolvedOut = (Resolve-Path -LiteralPath $out).Path
    $resolvedDist = (Resolve-Path -LiteralPath $distRoot).Path
    if (!$resolvedOut.StartsWith($resolvedDist + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw "Refusing to replace a path outside dist." }
    Remove-Item -LiteralPath $resolvedOut -Recurse -Force
}
New-Item -ItemType Directory -Force -Path (Join-Path $candidate "Tools"),(Join-Path $indexFiles "Tools") | Out-Null
$releaseFiles = @(
    @{ Source=(Join-Path $plugin "bin\Release\BrushModeler.dll"); Relative="BrushModeler.dll" },
    @{ Source=(Join-Path $plugin "BrushModeler.ini"); Relative="BrushModeler.ini" },
    @{ Source=(Join-Path $plugin "merge_xmodels.exe"); Relative="merge_xmodels.exe" },
    @{ Source=(Join-Path $plugin "README.md"); Relative="README.md" },
    @{ Source=(Join-Path $plugin "CHANGELOG.md"); Relative="CHANGELOG.md" },
    @{ Source=(Join-Path $plugin "THIRD_PARTY_NOTICES.md"); Relative="THIRD_PARTY_NOTICES.md" },
    @{ Source=(Join-Path $plugin "Tools\exportx.exe"); Relative="Tools\exportx.exe" }
)
foreach ($file in $releaseFiles) {
    $dest = Join-Path $candidate $file.Relative
    Copy-Item -LiteralPath $file.Source -Destination $dest
    Copy-Item -LiteralPath $dest -Destination (Join-Path $indexFiles $file.Relative)
}

$files = @()
Get-ChildItem -LiteralPath $candidate -File -Recurse | ForEach-Object {
    $relative = $_.FullName.Substring($candidate.Length + 1).Replace('\','/')
    $entry = [ordered]@{ path=$relative; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    if ($relative -eq "BrushModeler.ini") { $entry.config = $true }
    $files += $entry
}
$indexEntry = [ordered]@{
    name = "BrushModeler"
    displayName = "Brush Modeler"
    author = "Owen"
    version = $Version
    description = "Convert selected Radiant brushes and patches to XModels, and merge selected BO3 models."
    changes = "Uses database-only model lookup, preserves merged model transforms and derived materials, and improves model merge diagnostics."
    apps = @("radiant")
    apiVersion = 1
    files = $files
}
$indexPath = Join-Path $out "index-entry.json"
$indexEntry | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 $indexPath
$zip = Join-Path $distRoot "BrushModeler-$Version.zip"
Compress-Archive -Path $candidate -DestinationPath $zip -Force
Write-Host "Release candidate: $zip"
Write-Host "Browse plugin index entry: $indexPath"
Write-Host "Index files to copy into MTPlugins-Index: $(Join-Path $out 'index-files')"
