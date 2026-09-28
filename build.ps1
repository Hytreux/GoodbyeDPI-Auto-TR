param([string]$OutputDirectory = (Join-Path $PSScriptRoot 'dist'))
$ErrorActionPreference='Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio C++ Build Tools gerekiyor.' }
$install = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $install) { throw 'MSVC x64 derleyicisi bulunamadi.' }
$vcvars = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
$environmentLines = & $env:ComSpec /d /c ('call "' + $vcvars + '" >nul && set')
if ($LASTEXITCODE -ne 0) { throw 'MSVC ortami hazirlanamadi.' }
foreach ($line in $environmentLines) { $pair = $line -split '=',2; if ($pair.Count -eq 2 -and $pair[0] -notmatch '^=') { [Environment]::SetEnvironmentVariable($pair[0],$pair[1],'Process') } }
$compilerPathLine = $environmentLines | Where-Object { $_ -cmatch '^PATH=' } | Select-Object -First 1
if ($compilerPathLine) { $env:PATH = $compilerPathLine.Substring(5) }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$build = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$imgui=Join-Path $PSScriptRoot 'vendor\imgui-1.91.9b'
Push-Location $PSScriptRoot
try {
 & rc.exe /nologo ('/fo' + (Join-Path $build 'app.res')) app.rc
 if ($LASTEXITCODE -ne 0) { throw 'Kaynak derlemesi basarisiz.' }
 $sources=@('app.cpp','platform.cpp','tests.cpp',"$imgui\imgui.cpp","$imgui\imgui_draw.cpp","$imgui\imgui_tables.cpp","$imgui\imgui_widgets.cpp","$imgui\backends\imgui_impl_win32.cpp","$imgui\backends\imgui_impl_dx11.cpp")
 $exe=Join-Path $OutputDirectory 'GoodbyeDPI-Auto.exe'
 & cl.exe /nologo /std:c++17 /utf-8 /O2 /MT /EHsc /W4 /Brepro /DUNICODE /D_UNICODE ('/I' + $imgui) ('/Fo' + $build + '\') ('/Fe' + $exe) @sources (Join-Path $build 'app.res') /link /Brepro /SUBSYSTEM:WINDOWS /MANIFEST:NO /DYNAMICBASE /NXCOMPAT d3d11.lib dxgi.lib d3dcompiler.lib dwmapi.lib user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib shlwapi.lib winhttp.lib dnsapi.lib crypt32.lib comdlg32.lib windowscodecs.lib
 if ($LASTEXITCODE -ne 0) { throw 'C++ derlemesi basarisiz.' }
 Write-Output $exe
} finally { Pop-Location }
