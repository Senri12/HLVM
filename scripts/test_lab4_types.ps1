param(
    [string]$BuildDir = "build/lab4_user_types_real",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$buildPath = [System.IO.Path]::GetFullPath((Join-Path $projectRoot $BuildDir))

if (-not $SkipBuild) {
    & powershell -ExecutionPolicy Bypass -File (Join-Path $projectRoot "scripts\build_lab4_erlang.ps1") -BuildDir $BuildDir
    if ($LASTEXITCODE -ne 0) {
        throw "Lab 4 build failed"
    }
}

$env:LAB4_CLASSPATH = $BuildDir
$output = & erl -pa $BuildDir -noshell -s lab4_app demo -s init stop
if ($LASTEXITCODE -ne 0) {
    $output | Write-Host
    throw "Lab 4 demo failed"
}

$expected = @(
    "gcd(48, 18) = 6",
    "lcm(21, 6) = 42",
    "isPrime(97) = 1",
    "fib(10) = 55",
    "sumRange(1, 10) = 55",
    "notBool(true) = false",
    "nextChar('A') = `"B`"",
    "incByte(41) = 42",
    "echoLong(1234567890123) = 1234567890123",
    "echoString(`"hello`") = `"hello`"",
    "acceptMany(true, 'Z', 7, 1234567890123, `"ok`") = ok",
    "makeVec2i(12, 30) = Vec2i#0",
    "vecX(Vec2i#0) = 12",
    "vecY(Vec2i#0) = 30",
    "vecSum(Vec2i#0) = 42"
)

foreach ($line in $expected) {
    if ($output -notcontains $line) {
        $output | Write-Host
        throw "Missing expected output line: $line"
    }
}

$output | Write-Host
Write-Host "Lab 4 type bridge tests passed."
