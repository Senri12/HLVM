param(
    [string]$InputFile = "src/lab4_math.sl",
    [string]$BuildDir = "build/lab4_app"
)

$ErrorActionPreference = "Stop"
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$buildPath = [System.IO.Path]::GetFullPath((Join-Path $projectRoot $BuildDir))
$inputPath = [System.IO.Path]::GetFullPath((Join-Path $projectRoot $InputFile))
$jasmPath = Join-Path $buildPath "lab4_math.jasm"
$dgmlPath = Join-Path $buildPath "lab4_math.dgml"
$javaSource = Join-Path $projectRoot "lab4_erlang\priv\java\SlBridge.java"
$erlSources = Join-Path $projectRoot "lab4_erlang\src\*.erl"

New-Item -ItemType Directory -Force $buildPath | Out-Null

Write-Host "Compiling SimpleLang library to JVM class..."
& powershell -ExecutionPolicy Bypass -File (Join-Path $projectRoot "tools\remote-parser.ps1") `
    -Target jvm `
    -InputFile $inputPath `
    -AsmOutput $jasmPath `
    -ParseTreeOutput $dgmlPath `
    -RemoteRunTimeoutSeconds 180
if ($LASTEXITCODE -ne 0) {
    throw "SimpleLang JVM compilation failed"
}

Write-Host "Compiling Java bridge..."
& javac -cp $buildPath -d $buildPath $javaSource
if ($LASTEXITCODE -ne 0) {
    throw "Java bridge compilation failed"
}

Write-Host "Compiling Erlang modules..."
$erlFiles = Get-ChildItem -LiteralPath (Join-Path $projectRoot "lab4_erlang\src") -Filter "*.erl" | ForEach-Object { $_.FullName }
& erlc -o $buildPath $erlFiles
if ($LASTEXITCODE -ne 0) {
    throw "Erlang compilation failed"
}

Write-Host "Lab 4 build is ready in $buildPath"
Write-Host "Demo:        `$env:LAB4_CLASSPATH='$BuildDir'; erl -pa $BuildDir -noshell -s lab4_app demo -s init stop"
Write-Host "Interactive: `$env:LAB4_CLASSPATH='$BuildDir'; erl -pa $BuildDir -noshell -s lab4_app main -s init stop"
