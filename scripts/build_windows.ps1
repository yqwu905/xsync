param(
    [string]$QtDir = $env:QT6_DIR,
    [string]$OpenSslDir = $env:OPENSSL_ROOT_DIR,
    [string]$BuildDir = "build-win64",
    [string]$DistDir = "dist\xsync"
)

$ErrorActionPreference = "Stop"

if (-not $QtDir) {
    $candidates = @(
        "C:\Qt\6.8.3\msvc2022_64",
        "C:\Qt\6.8.2\msvc2022_64",
        "C:\Qt\6.7.3\msvc2022_64"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $QtDir = $c; break }
    }
}
if (-not $QtDir -or -not (Test-Path $QtDir)) {
    throw "Qt6 MSVC directory not found. Pass -QtDir C:\Qt\6.8.3\msvc2022_64 or set QT6_DIR."
}

if (-not $OpenSslDir) {
    foreach ($c in @("C:\Program Files\OpenSSL-Win64", "C:\Program Files\OpenSSL")) {
        if (Test-Path $c) { $OpenSslDir = $c; break }
    }
}

Write-Host "Using Qt6: $QtDir" -ForegroundColor Cyan
if ($OpenSslDir) { Write-Host "Using OpenSSL: $OpenSslDir" -ForegroundColor Cyan }

$cmakeArgs = @(
    "-S", ".",
    "-B", $BuildDir,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_PREFIX_PATH=$QtDir"
)
if ($OpenSslDir) {
    $cmakeArgs += "-DOPENSSL_ROOT_DIR=$OpenSslDir"
}

cmake @cmakeArgs
cmake --build $BuildDir --config Release --parallel

Write-Host "Running unit and memory stability tests..." -ForegroundColor Cyan
$env:PATH = "$QtDir\bin;$OpenSslDir\bin;$env:PATH"
$env:QT_QPA_PLATFORM = "offscreen"
ctest --test-dir $BuildDir --output-on-failure

Write-Host "Deploying portable Windows package to $DistDir..." -ForegroundColor Cyan
if (Test-Path $DistDir) { Remove-Item -Recurse -Force $DistDir }
New-Item -ItemType Directory -Force -Path $DistDir | Out-Null
Copy-Item "$BuildDir\xsync.exe" "$DistDir\xsync.exe"

& "$QtDir\bin\windeployqt.exe" `
    --release `
    --no-translations `
    --no-opengl-sw `
    --no-system-d3d-compiler `
    "$DistDir\xsync.exe"

# Copy OpenSSL 3 runtime DLLs if linked dynamically
if ($OpenSslDir) {
    foreach ($dll in @("libcrypto-3-x64.dll", "libssl-3-x64.dll")) {
        $src = Join-Path "$OpenSslDir\bin" $dll
        if (-not (Test-Path $src)) { $src = Join-Path $OpenSslDir $dll }
        if (Test-Path $src) { Copy-Item $src "$DistDir\$dll" -Force }
    }
}

$zipPath = "dist\xsync-windows-x64-portable.zip"
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path "$DistDir\*" -DestinationPath $zipPath

Write-Host "Build complete!" -ForegroundColor Green
Write-Host "  Portable executable: $DistDir\xsync.exe"
Write-Host "  Portable archive:    $zipPath"
