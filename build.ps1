param(
    [string]$OutDir = "dist"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $Root

try {
    $vcvarsCandidates = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars32.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars32.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars32.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars32.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars32.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
    )

    $vcvars = $null
    foreach ($candidate in $vcvarsCandidates) {
        if (Test-Path $candidate) {
            $vcvars = $candidate
            break
        }
    }

    if (-not $vcvars) {
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path $vswhere) {
            $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($installPath) {
                $candidate = Join-Path $installPath "VC\Auxiliary\Build\vcvars32.bat"
                if (Test-Path $candidate) {
                    $vcvars = $candidate
                }
            }
        }
    }

    if (-not $vcvars) {
        throw "Could not find vcvars32.bat (Visual Studio C++ x86 build tools required)."
    }

    $buildDir = Join-Path $Root "build"
    $distDir  = Join-Path $Root $OutDir
    $stageDir = Join-Path $buildDir "stage"

    if (Test-Path $buildDir) { Remove-Item $buildDir -Recurse -Force }
    if (Test-Path $distDir)  { Remove-Item $distDir -Recurse -Force }

    New-Item -ItemType Directory -Path $buildDir, $distDir, $stageDir -Force | Out-Null

    $srcFile = Join-Path $Root "src\chat_filter.c"
    $defFile = Join-Path $Root "src\msimg32.def"
    $outDll  = Join-Path $stageDir "msimg32.dll"

    $cmdLine = "`"$vcvars`" && cd /d `"$buildDir`" && cl.exe /nologo /O2 /MT /W3 /LD `"$srcFile`" /link /DEF:`"$defFile`" /OUT:`"$outDll`" user32.lib kernel32.lib"
    cmd.exe /c $cmdLine
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $outDll)) {
        throw "Compilation failed with exit code $LASTEXITCODE"
    }

    Copy-Item (Join-Path $Root "muted_players.txt") (Join-Path $stageDir "muted_players.txt") -Force
    Copy-Item (Join-Path $Root "README.md")         (Join-Path $stageDir "README.md") -Force

    $zipPath = Join-Path $distDir "bc2-chat-filter-windows.zip"
    Compress-Archive -Path (Join-Path $stageDir "*") -DestinationPath $zipPath -Force

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    $dllBytes = [System.IO.File]::ReadAllBytes($outDll)
    $zipBytes = [System.IO.File]::ReadAllBytes($zipPath)
    $dllHash = ([System.BitConverter]::ToString($sha256.ComputeHash($dllBytes))).Replace("-", "").ToLowerInvariant()
    $zipHash = ([System.BitConverter]::ToString($sha256.ComputeHash($zipBytes))).Replace("-", "").ToLowerInvariant()

    Copy-Item $outDll (Join-Path $distDir "msimg32.dll") -Force

    $shaLines = @(
        "$zipHash  bc2-chat-filter-windows.zip",
        "$dllHash  msimg32.dll"
    )
    [System.IO.File]::WriteAllLines((Join-Path $distDir "SHA256.txt"), $shaLines)

    Write-Host "Build succeeded:"
    Write-Host "  DLL: $distDir\msimg32.dll ($dllHash)"
    Write-Host "  ZIP: $zipPath ($zipHash)"
} finally {
    Pop-Location
}
