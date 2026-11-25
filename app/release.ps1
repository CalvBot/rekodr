# `build release` runs this after the build. It writes dist\rekodr-<version>.exe, the same exe as
# rekodr-setup.exe (the download for new players), its .pdb (for reading crash dumps from that version) and
# latest.txt, the manifest the update check reads (update.cpp):
#   version <version>
#   url <where rekodr-<version>.exe is downloaded from>
#   size <bytes>
#   sha256 <hex>
#   sig <hex>
# TODO: set $base to where releases are hosted (UPDATE_MANIFEST_URL in update.cpp is $base/latest.txt), and sign:
# the sig line is an ECDSA P-256 signature with the release key, r then s as 128 hex digits, over the SHA-256 of
# every byte before it.
param([string]$Build = "$PSScriptRoot\build", [string]$Dist = "$PSScriptRoot\..\dist")
$ErrorActionPreference = "Stop"

$base = "TODO"
$version = [regex]::Match((Get-Content "$PSScriptRoot\version.h" -Raw), 'REKODR_VERSION "([^"]+)"').Groups[1].Value
if (-not $version) { throw "version.h has no REKODR_VERSION" }
$built = (Get-Item "$Build\rekodr.exe").VersionInfo.ProductVersion
if ($built -notlike "$version*") { throw "$Build\rekodr.exe says $built, version.h says $version" }
if (-not (Select-String -Path "$PSScriptRoot\whatsnew.txt" -Pattern "^# $([regex]::Escape($version))\s*$" -Quiet)) {
    throw "whatsnew.txt has no '# $version' notes"
}

New-Item -ItemType Directory -Force $Dist | Out-Null
$exe = Join-Path $Dist "rekodr-$version.exe"
Copy-Item "$Build\rekodr.exe" $exe
Copy-Item "$Build\rekodr.exe" (Join-Path $Dist "rekodr-setup.exe") # the same exe; by this name it installs
Copy-Item "$Build\rekodr.pdb" (Join-Path $Dist "rekodr-$version.pdb")
$size = (Get-Item $exe).Length
$sha = (Get-FileHash $exe -Algorithm SHA256).Hash.ToLower()
# LF line ends and no BOM: the signature covers these exact bytes
$text = "version $version`nurl $base/rekodr-$version.exe`nsize $size`nsha256 $sha`n"
[IO.File]::WriteAllText((Join-Path $Dist "latest.txt"), $text)
Write-Host "$exe ($size bytes), rekodr-setup.exe and latest.txt, not signed yet"
