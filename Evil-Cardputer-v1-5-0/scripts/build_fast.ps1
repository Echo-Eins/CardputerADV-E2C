param(
    [ValidateSet("disabled", "enabled", "opi")]
    [string]$Psram = "disabled",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$projectDir = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $projectDir "Evil-Cardputer-v1-5-0.ino"))) {
    throw "Project root not found from script path: $projectDir"
}

$workspaceDir = Split-Path -Parent $projectDir
$buildPath = Join-Path $workspaceDir ("_build_fast_" + $Psram)
$outPath = Join-Path $buildPath "out"
$releaseDir = Join-Path $workspaceDir "_bin_release"

$fqbn = "m5stack:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=custom,PSRAM=$Psram"

New-Item -ItemType Directory -Path $buildPath, $outPath, $releaseDir -Force | Out-Null

$args = @(
    "compile"
    "--fqbn", $fqbn
    "--build-path", $buildPath
    "--output-dir", $outPath
    $projectDir
)
if ($Clean) {
    $args = @("compile", "--clean") + $args[1..($args.Length - 1)]
}

Write-Host "[build_fast] FQBN: $fqbn"
Write-Host "[build_fast] Build path: $buildPath"

& arduino-cli @args
if ($LASTEXITCODE -ne 0) {
    throw "arduino-cli compile failed with exit code $LASTEXITCODE"
}

$base = "Evil-Cardputer-v1-5-0.ino"
$srcBin = Join-Path $outPath ($base + ".bin")
$srcMerged = Join-Path $outPath ($base + ".merged.bin")

Copy-Item $srcBin (Join-Path $releaseDir ($base + ".bin")) -Force
Copy-Item $srcMerged (Join-Path $releaseDir ($base + ".merged.bin")) -Force
Copy-Item $srcBin (Join-Path $releaseDir ("Evil-Cardputer-v1-5-0-psram-" + $Psram + ".bin")) -Force
Copy-Item $srcMerged (Join-Path $releaseDir ("Evil-Cardputer-v1-5-0-psram-" + $Psram + ".merged.bin")) -Force

Write-Host "[build_fast] Done. Binaries copied to $releaseDir"
