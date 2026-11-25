param(
    [string] $Clang = 'clang++.exe',
    [string] $Output = (Join-Path $PSScriptRoot '..')
)

$ErrorActionPreference = 'Stop'
$outputDirectory = [IO.Path]::GetFullPath($Output)
$null = New-Item -ItemType Directory -Force -Path $outputDirectory
$native = Join-Path $outputDirectory 'StartMenuGrid.Layout.dll'
$exports = Join-Path $PSScriptRoot 'StartMenuGrid.Layout.def'
$layoutSource = Join-Path $PSScriptRoot 'StartMenuGrid.Layout.cpp'
& $Clang --target=x86_64-w64-mingw32 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -std=c++20 -O2 -shared -static '-Wl,--exclude-all-symbols' -o $native $layoutSource $exports -lole32 -loleaut32 -lruntimeobject -lgdi32 -luser32 -ladvapi32 -luuid
if ($LASTEXITCODE -ne 0) { throw "Native compiler exited with code $LASTEXITCODE." }

$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$manifest = Join-Path $PSScriptRoot 'StartMenuGrid.manifest'
$panel = Join-Path $PSScriptRoot 'StartMenuGrid.cs'
$runtime = Join-Path $PSScriptRoot 'StandaloneRuntime.cs'
$exe = Join-Path $outputDirectory 'StartMenuGrid.exe'
& $compiler /nologo /codepage:65001 /optimize+ /target:winexe /platform:x64 /r:System.Windows.Forms.dll /r:System.Drawing.dll /r:Microsoft.CSharp.dll "/win32manifest:$manifest" "/out:$exe" $panel $runtime
if ($LASTEXITCODE -ne 0) { throw "C# compiler exited with code $LASTEXITCODE." }
