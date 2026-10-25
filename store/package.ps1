# Makes the release files from a finished .\build.ps1, in build\:
#   FastFTP360-<version>.zip  for the GitHub release: the XEX and the network test's PC app.
#   FastFTP360-<version>.7z   for the Xbox 360 Homebrew Store: the XEX, readme.txt and LICENSE.txt,
#                             which the store unpacks into Apps\FastFTP360\.
#   FastFTP360-<version>.ini  the store entry for that 7z (from consolemods.ini).
# The version comes from the app's own screen header in shared\ui.cpp.
param([string]$SevenZip)
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot)
try {
    $version = [regex]::Match((Get-Content -Raw shared\ui.cpp), 'disk tests  v(\d+\.\d+\.\d+)').Groups[1].Value
    if (!$version) { throw 'No version found in shared\ui.cpp.' }
    if ((Get-Content -Raw windows\Program.cs) -notmatch "network test v$([regex]::Escape($version))") { throw "windows\Program.cs doesn't say v$version like shared\ui.cpp." }
    $xex = 'build\xbox\FastFTP360.xex'; $pc = 'build\windows\FastFTP360-PC.exe'
    foreach ($file in $xex, $pc) { if (!(Test-Path -LiteralPath $file)) { throw "$file is missing. Run .\build.ps1 first." } }
    if (!$SevenZip) { $SevenZip = (Get-Command 7z -ErrorAction SilentlyContinue).Source }
    if (!$SevenZip) { $SevenZip = Join-Path $env:ProgramFiles '7-Zip\7z.exe' }
    if (!(Test-Path -LiteralPath $SevenZip)) { throw '7-Zip is needed. Install it, or pass -SevenZip with the path to 7z.exe.' }
    $name = "FastFTP360-$version"
    $out = (Resolve-Path build).Path
    # 7-Zip adds to an archive that exists, so the old files go first.
    foreach ($file in "$out\$name.zip", "$out\$name.7z", "$out\$name.ini") { if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file } }
    $stage = "build\store\$name"
    New-Item -ItemType Directory -Force $stage | Out-Null
    Copy-Item -LiteralPath $xex -Destination $stage -Force
    Copy-Item -LiteralPath LICENSE -Destination "$stage\LICENSE.txt" -Force
    (Get-Content -Raw store\readme.txt).Replace('@VERSION@', $version) | Set-Content -NoNewline -Encoding ascii "$stage\readme.txt"
    # The files go at the top of the archives, with no folder around them:
    # the store unpacks the 7z into the entry's path itself.
    & $SevenZip a -tzip -bso0 "$out\$name.zip" (Resolve-Path $xex).Path (Resolve-Path $pc).Path
    if ($LASTEXITCODE) { throw '7-Zip failed on the zip.' }
    # Default 7-Zip settings (LZMA2), as the 7z files on the ConsoleMods repo use.
    Push-Location $stage
    try { & $SevenZip a -t7z -bso0 "$out\$name.7z" * } finally { Pop-Location }
    if ($LASTEXITCODE) { throw '7-Zip failed on the 7z.' }
    $size = (Get-Item -LiteralPath "$out\$name.7z").Length
    $sizeText = if ($size -ge 1MB) { ($size / 1MB).ToString('0.#', [Globalization.CultureInfo]::InvariantCulture) + 'MB' } else { [string][math]::Ceiling($size / 1KB) + 'KB' }
    (Get-Content -Raw store\consolemods.ini).Replace('@VERSION@', $version).Replace('@SIZE@', $sizeText) | Set-Content -NoNewline -Encoding ascii "$out\$name.ini"
    Write-Host "Made build\$name.zip, build\$name.7z ($sizeText) and build\$name.ini."
    Write-Host "Attach the zip and the 7z to the GitHub release $version. The ini's dataurl points at the 7z there."
} finally { Pop-Location }
