param([switch]$HostOnly, [switch]$Tests)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer/vswhere is required.' }
    $vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vsPath) { throw 'Install Visual Studio C++ build tools.' }
    $csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    if (!(Test-Path -LiteralPath $csc)) { throw '.NET Framework compiler not found.' }
    New-Item -ItemType Directory -Force build\windows,build\host,build\host\lan | Out-Null
    & $csc /nologo /optimize+ /target:winexe /out:build\windows\NetBench360-PC.exe /reference:System.Windows.Forms.dll /reference:System.Drawing.dll /reference:System.Core.dll windows\Engine.cs windows\Program.cs
    if ($LASTEXITCODE) { throw 'Windows compilation failed.' }
    if (!$HostOnly) {
        $msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
        & $msbuild xbox\NetBench360.vcxproj /t:Build /p:Configuration=Release '/p:Platform=Xbox 360' /v:normal /nologo
        if ($LASTEXITCODE) { throw 'Xbox compilation failed. Check the SDK and RXDK integration.' }
    }
    if ($Tests) {
        & $csc /nologo /optimize+ /target:exe /out:build\host\client-cli.exe windows\Engine.cs tests\client_cli.cs
        if ($LASTEXITCODE) { throw 'Test client compilation failed.' }
        $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
        # Only compiler commands are chained; no filesystem deletion or movement.
        & $env:ComSpec /d /s /c ('"' + $vcvars + '" >nul && cl /nologo /EHsc /O2 /W4 tests\host_server.cpp shared\bench.cpp /Febuild\host\host-server.exe /Fobuild\host\ /link ws2_32.lib && cl /nologo /EHsc /O2 /W4 tests\ui_preview.cpp shared\ui.cpp /Febuild\host\ui-preview.exe /Fobuild\host\ && cl /nologo /EHsc /O2 /W4 /DNB_TEST_LAN_OPTIONS /Itests /FIlan_socket_shim.h tests\host_server.cpp tests\lan_socket_shim.cpp shared\bench.cpp /Febuild\host\lan-host-server.exe /Fobuild\host\lan\ /link ws2_32.lib')
        if ($LASTEXITCODE) { throw 'Native host test compilation failed.' }
        & python tests\protocol.py
        if ($LASTEXITCODE) { throw 'Protocol tests failed.' }
    }
    Write-Host 'Build complete. No console deployment was requested.'
} finally { Pop-Location }
