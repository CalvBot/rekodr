# Builds the clips window: win\dist\clipback-ui.exe
# Needs the .NET 9 SDK.
#
#   .\build.ps1                  framework-dependent (needs the .NET 9 Desktop Runtime, small exe)
#   .\build.ps1 -SelfContained   bundles the runtime (no install needed, much bigger exe)

param([switch]$SelfContained)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$dist = "$root\dist"
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
$sc = if ($SelfContained) { "true" } else { "false" }
dotnet publish "$root\clipback-ui.csproj" -c Release -o $dist -p:SelfContained=$sc --nologo -v q
if ($LASTEXITCODE) { throw "Build failed (exit $LASTEXITCODE)" }
Get-ChildItem $dist -Filter *.pdb | Remove-Item
$exe = Get-Item "$dist\clipback-ui.exe"
Write-Host ("Built {0} ({1:N1} MB)" -f $exe.FullName, ($exe.Length / 1MB)) -ForegroundColor Green
