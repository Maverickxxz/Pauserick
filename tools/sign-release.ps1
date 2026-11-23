# Signs a Pauserick.exe for the built-in updater: writes Pauserick.exe.sig next to it.
# Used on the exe that GitHub Actions built (see RELEASING.md); needs no compiler, only
# Windows' own crypto. The version is read from the exe itself.
#
#   powershell -ExecutionPolicy Bypass -File tools\sign-release.ps1 -Exe <Pauserick.exe> [-Key <private.key>]
#
# Signature: ECDSA P-256 over SHA-256("PAUSERICK-UPDATE\n" + version + "\n" + exe bytes),
# the same as tools\pksign.c. The private key never leaves this PC.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$Key = "$env:LOCALAPPDATA\Pauserick-signing\pauserick-update-private.key"
)
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Key)) { throw "Signing key not found: $Key" }
if (-not (Test-Path -LiteralPath $Exe)) { throw "File not found: $Exe" }
$Exe = (Resolve-Path -LiteralPath $Exe).Path
$version = (Get-Item -LiteralPath $Exe).VersionInfo.FileVersion
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "$Exe has no Pauserick version (found '$version')" }

$bytes = [IO.File]::ReadAllBytes($Exe)
if ($bytes.Length -lt 1024 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { throw "$Exe is not a Windows program" }
$prefix = [Text.Encoding]::ASCII.GetBytes("PAUSERICK-UPDATE`n$version`n")
$sha = [Security.Cryptography.SHA256]::Create()
[void]$sha.TransformBlock($prefix, 0, $prefix.Length, $null, 0)
[void]$sha.TransformFinalBlock($bytes, 0, $bytes.Length)
$digest = $sha.Hash

$cng = [Security.Cryptography.CngKey]::Import([IO.File]::ReadAllBytes($Key), [Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
$ecdsa = New-Object Security.Cryptography.ECDsaCng $cng
$sig = $ecdsa.SignHash($digest)
if ($sig.Length -ne 64 -or -not $ecdsa.VerifyHash($digest, $sig)) { throw "Signing failed" }
$cng.Dispose()

$hex = -join ($sig | ForEach-Object { $_.ToString('x2') })
$out = Join-Path (Split-Path -Parent $Exe) 'Pauserick.exe.sig'
[IO.File]::WriteAllText($out, "pauserick-update-signature v1`nversion $version`nsignature $hex`n", [Text.Encoding]::ASCII)

$hash = -join ([Security.Cryptography.SHA256]::Create().ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') })
Write-Host "Signed Pauserick $version"
Write-Host "  exe:       $Exe"
Write-Host "  SHA-256:   $hash   (must match the one in the draft release)"
Write-Host "  signature: $out"
Write-Host "Attach Pauserick.exe.sig to the draft release, then publish it."
