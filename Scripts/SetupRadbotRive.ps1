param(
    [string]$ProjectRoot = "",
    [switch]$SkipSubmoduleUpdate
)

$arguments = @("-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "BuildRadbotRive.ps1"), "-BootstrapWin64")

if (-not [string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $arguments += @("-ProjectRoot", $ProjectRoot)
}

if ($SkipSubmoduleUpdate) {
    $arguments += "-SkipSubmoduleUpdate"
}

& powershell.exe @arguments
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
