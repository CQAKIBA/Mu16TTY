param(
    [string]$CMake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    [string]$PortAudioDirectory
)

$ErrorActionPreference = "Stop"
$project = Split-Path -Parent $PSScriptRoot
if (-not $PortAudioDirectory) {
    throw "Specify -PortAudioDirectory containing libportaudio64bit.dll and README.md."
}
$audioFiles = 'libportaudio64bit.dll', 'README.md'
foreach ($file in $audioFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $PortAudioDirectory $file) -PathType Leaf)) {
        throw "PortAudio distribution file missing: $file ($PortAudioDirectory)"
    }
}

$build = Join-Path $project 'build\native'
& $CMake -S (Join-Path $project 'native') -B $build -A x64
if ($LASTEXITCODE -ne 0) { throw 'Native station configure failed' }
& $CMake --build $build --config Release --target mu16tty_station
if ($LASTEXITCODE -ne 0) { throw 'Native station build failed' }

$dist = Join-Path $project 'dist'
New-Item -ItemType Directory -Path $dist -Force | Out-Null
$zip = Join-Path $dist 'Mu16TTY-native-windows-x64.zip'
$stage = Join-Path (Join-Path $project 'build') ("native-package-" + [guid]::NewGuid())
$app = Join-Path $stage 'Mu16TTY-native'
try {
    $audio = Join-Path $app 'runtime\portaudio'
    New-Item -ItemType Directory -Path $audio -Force | Out-Null
    foreach ($file in $audioFiles) {
        Copy-Item -LiteralPath (Join-Path $PortAudioDirectory $file) -Destination $audio
    }
    foreach ($file in 'Mu16TTY-native.exe', 'mu16tty_modem.dll') {
        Copy-Item -LiteralPath (Join-Path $build "Release\$file") -Destination $app
    }
    $web = Join-Path $app 'web'
    New-Item -ItemType Directory -Path $web | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $project 'web') -File |
        Where-Object Extension -In '.html', '.css', '.js' |
        Copy-Item -Destination $web
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README-native.txt') -Destination (Join-Path $app 'README.txt')
    Copy-Item -LiteralPath (Join-Path $project 'LICENSE') -Destination (Join-Path $app 'LICENSE')
    Copy-Item -LiteralPath (Join-Path $project 'THIRD_PARTY_NOTICES.md') -Destination $app
    Copy-Item -LiteralPath (Join-Path $project 'licenses') -Destination $app -Recurse
    Copy-Item -LiteralPath (Join-Path $project 'docs\Mu16TTY通信方式仕様書.md') -Destination $app
    $stagedZip = Join-Path $stage 'Mu16TTY-native-windows-x64.zip'
    Compress-Archive -LiteralPath $app -DestinationPath $stagedZip
    Move-Item -LiteralPath $stagedZip -Destination $zip -Force
    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -LiteralPath "$zip.sha256" -Value "$hash  $(Split-Path -Leaf $zip)" -Encoding ascii
    Write-Output $zip
} finally {
    if (Test-Path -LiteralPath $stage) {
        $resolved = Get-Item -LiteralPath $stage -Force
        if ($resolved.FullName -eq $stage -and -not ($resolved.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            Remove-Item -LiteralPath $resolved.FullName -Recurse -Force
        }
    }
}
