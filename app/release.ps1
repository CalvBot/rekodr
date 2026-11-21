# `build release` runs this after the build. It writes dist\clipback-<version>.exe, the same exe as
# clipback-setup.exe (the download for new players), its .pdb (for reading crash dumps from that version) and
# latest.txt, the manifest the update check reads (update.cpp):
#   version <version>
#   url <where clipback-<version>.exe is downloaded from>
#   size <bytes>
#   sha256 <hex>
#   sig <hex>
# TODO: set $base to where releases are hosted (UPDATE_MANIFEST_URL in update.cpp is $base/latest.txt), and sign:
# the sig line is an ECDSA P-256 signature with the release key, r then s as 128 hex digits, over the SHA-256 of
# every byte before it.
param([string]$Build = "$PSScriptRoot\build", [string]$Dist = "$PSScriptRoot\..\dist")
$ErrorActionPreference = "Stop"

$base = "TODO"
$version = [regex]::Match((Get-Content "$PSScriptRoot\version.h" -Raw), 'CLIPBACK_VERSION "([^"]+)"').Groups[1].Value
if (-not $version) { throw "version.h has no CLIPBACK_VERSION" }
$built = (Get-Item "$Build\clipback.exe").VersionInfo.ProductVersion
if ($built -notlike "$version*") { throw "$Build\clipback.exe says $built, version.h says $version" }

New-Item -ItemType Directory -Force $Dist | Out-Null
$exe = Join-Path $Dist "clipback-$version.exe"
Copy-Item "$Build\clipback.exe" $exe
Copy-Item "$Build\clipback.exe" (Join-Path $Dist "clipback-setup.exe") # the same exe; by this name it installs
Copy-Item "$Build\clipback.pdb" (Join-Path $Dist "clipback-$version.pdb")
$size = (Get-Item $exe).Length
$sha = (Get-FileHash $exe -Algorithm SHA256).Hash.ToLower()
# LF line ends and no BOM: the signature covers these exact bytes
$text = "version $version`nurl $base/clipback-$version.exe`nsize $size`nsha256 $sha`n"
[IO.File]::WriteAllText((Join-Path $Dist "latest.txt"), $text)
Write-Host "$exe ($size bytes), clipback-setup.exe and latest.txt, not signed yet"
