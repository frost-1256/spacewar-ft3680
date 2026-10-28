# Sign the FT3680 package with a self-signed test certificate.
# Run in normal PowerShell:  powershell -ExecutionPolicy Bypass -File sign_driver.ps1
$ErrorActionPreference = 'Stop'
$pkg = "$env:USERPROFILE\Desktop\ft3680-driver\ARM64\Release"
$kit = 'I:\Program Files\Windows Kits\10\bin\10.0.28000.0'
$signtool = "$kit\x64\signtool.exe"
$inf2cat  = "$kit\x86\inf2cat.exe"
if (-not (Test-Path "$pkg\FT3680Touch.sys")) { throw "package not found in $pkg" }

# 1. certificate (create once, reuse after)
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq 'CN=spacewar test' } | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=spacewar test' `
        -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(5)
    Write-Host "created cert $($cert.Thumbprint)"
} else { Write-Host "reuse cert $($cert.Thumbprint)" }

# 2. sign the .sys (embedded signature)
& $signtool sign /fd sha256 /s My /n 'spacewar test' "$pkg\FT3680Touch.sys"
if ($LASTEXITCODE -ne 0) { throw 'signtool failed on .sys' }

# 3. catalog from the signed files, then sign the catalog
Remove-Item "$pkg\FT3680Touch.cat" -ErrorAction SilentlyContinue
& $inf2cat /driver:$pkg /os:10_ARM64 /uselocaltime
if ($LASTEXITCODE -ne 0) { throw 'inf2cat failed' }
& $signtool sign /fd sha256 /s My /n 'spacewar test' "$pkg\FT3680Touch.cat"
if ($LASTEXITCODE -ne 0) { throw 'signtool failed on .cat' }

& $signtool verify /pa /v "$pkg\FT3680Touch.sys" | Select-Object -First 6
Write-Host 'SIGNED OK. Now: DISM Add-Driver (with /ForceUnsigned is fine), boot, check ft3680.log.'
