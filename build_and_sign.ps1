# Build + sign in one go. Run INSIDE the EWDK shell (after LaunchBuildEnv):
#   powershell -ExecutionPolicy Bypass -File build_and_sign.ps1
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$pkg = "$PSScriptRoot\ARM64\Release"
$kit = 'I:\Program Files\Windows Kits\10\bin\10.0.28000.0'
$signtool = "$kit\x64\signtool.exe"

Write-Host '=== BUILD ==='
& msbuild FT3680Touch.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=ARM64 /p:SignMode=Off /p:LinkIntegrityCheck=false /v:minimal
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

Write-Host '=== SIGN ==='
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq 'CN=spacewar test' } | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=spacewar test' `
        -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(5)
}
& $signtool sign /fd sha256 /s My /n 'spacewar test' "$pkg\FT3680Touch.sys"
if ($LASTEXITCODE -ne 0) { throw 'signtool failed' }

$f = Get-Item "$pkg\FT3680Touch.sys"
Write-Host ("DONE: signed .sys = {0} bytes ({1})" -f $f.Length, $f.LastWriteTime)
Write-Host 'Next: copy_driver.bat (admin), then boot.'
