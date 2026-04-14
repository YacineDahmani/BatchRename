$ErrorActionPreference = "Stop"

if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$exeCandidates = @(
    (Join-Path $repoRoot "renamer.exe"),
    (Join-Path $repoRoot "renamer")
)
$exePath = $null

foreach ($candidate in $exeCandidates) {
    if (Test-Path $candidate) {
        $exePath = $candidate
        break
    }
}

Assert-True ($null -ne $exePath) "renamer executable not found. Build first with make."

$workRoot = Join-Path $repoRoot ".tmp_test_run"
if (Test-Path $workRoot) {
    Remove-Item -Recurse -Force $workRoot
}

New-Item -ItemType Directory -Path (Join-Path $workRoot "regex") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $workRoot "chain") | Out-Null

Set-Content -Path (Join-Path $workRoot "regex\photo_001.jpg") -Value "a"
Set-Content -Path (Join-Path $workRoot "regex\photo_002.jpg") -Value "b"
Set-Content -Path (Join-Path $workRoot "regex\notes.txt") -Value "c"
Set-Content -Path (Join-Path $workRoot "chain\A.jpg") -Value "a"
Set-Content -Path (Join-Path $workRoot "chain\photo_001.jpg") -Value "b"

Push-Location $workRoot
try {
    $regexOutput = & $exePath --dryrun --yes --regex '^photo_[0-9]{3}\.jpg$' regex batch_ 2 2>&1 | Out-String
    Assert-True ($LASTEXITCODE -eq 0) "anchored regex dry run failed"
    Assert-True ($regexOutput -match "Dry run: 2 file\(s\)") "anchored regex matched an unexpected number of files"

    $invalidOutFile = Join-Path $workRoot "invalid.out"
    $invalidErrFile = Join-Path $workRoot "invalid.err"
    $invalidRun = Start-Process -FilePath $exePath -NoNewWindow -Wait -PassThru -ArgumentList @("--dryrun", "--yes", "--regex", "*photo", "regex", "batch_", "2") -RedirectStandardOutput $invalidOutFile -RedirectStandardError $invalidErrFile
    $invalidOutput = (Get-Content $invalidOutFile -Raw) + (Get-Content $invalidErrFile -Raw)
    Assert-True ($invalidRun.ExitCode -ne 0) "invalid regex quantifier should fail"
    Assert-True ($invalidOutput -match "Invalid regex pattern") "invalid regex error message missing"

    & $exePath --yes chain .jpg photo_ 3 | Out-Null
    Assert-True ($LASTEXITCODE -eq 0) "two-phase rename chain scenario failed"

    $chainNames = Get-ChildItem (Join-Path $workRoot "chain") | Select-Object -ExpandProperty Name
    Assert-True (($chainNames -contains "photo_001.jpg") -and ($chainNames -contains "photo_002.jpg")) "chain rename targets not produced"

    & $exePath undo --yes | Out-Null
    Assert-True ($LASTEXITCODE -eq 0) "undo after chain rename failed"

    $restoredNames = Get-ChildItem (Join-Path $workRoot "chain") | Select-Object -ExpandProperty Name
    Assert-True (($restoredNames -contains "A.jpg") -and ($restoredNames -contains "photo_001.jpg")) "undo did not restore expected files"

    Write-Host "All basic regression checks passed."
}
finally {
    Pop-Location
    if (Test-Path $workRoot) {
        Remove-Item -Recurse -Force $workRoot
    }
}
