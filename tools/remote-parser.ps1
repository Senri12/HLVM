param(
    [string]$InputFile = "input.txt",
    [string]$AsmOutput = "build/program.asm",
    [string]$ParseTreeOutput = "build/parse_tree.dgml",
    [switch]$ParseOnly,
    [switch]$DebugProgress,
    [string]$ProgressOutput = "",
    [ValidateSet("tacvm13", "tac", "jvm")]
    [string]$Target = "tacvm13",
    [int]$RemoteRunTimeoutSeconds = 0,
    [string]$RemoteHost = "localhost",
    [int]$RemotePort = 5555,
    [string]$RemoteUser = "user",
    [string]$RemotePassword = "student",
    [string]$SshExe = "C:\Windows\System32\OpenSSH\ssh.exe",
    [string]$ScpExe = "C:\Windows\System32\OpenSSH\scp.exe"
)

$ErrorActionPreference = "Stop"
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))

function Resolve-AbsolutePath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [switch]$MustExist
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        $resolved = [System.IO.Path]::GetFullPath($Path)
    } else {
        $resolved = [System.IO.Path]::GetFullPath((Join-Path $projectRoot $Path))
    }

    if ($MustExist -and -not (Test-Path -LiteralPath $resolved)) {
        throw "Path not found: $resolved"
    }

    return $resolved
}

$inputPath = Resolve-AbsolutePath -Path $InputFile -MustExist
$asmPath = Resolve-AbsolutePath -Path $AsmOutput
$dgmlPath = Resolve-AbsolutePath -Path $ParseTreeOutput
$sshExePath = Resolve-AbsolutePath -Path $SshExe -MustExist
$scpExePath = Resolve-AbsolutePath -Path $ScpExe -MustExist

New-Item -ItemType Directory -Force (Split-Path -Parent $asmPath) | Out-Null
New-Item -ItemType Directory -Force (Split-Path -Parent $dgmlPath) | Out-Null

$sessionId = [guid]::NewGuid().ToString("N")
$remoteDir = "/home/user/projects/spo3_remote_$sessionId"
$remoteInputName = [System.IO.Path]::GetFileName($inputPath)
$remoteAsmPath = "out/program.asm"
$remoteDgmlPath = "out/parse_tree.dgml"
$remoteProgressPath = "out/progress.log"
$sshTarget = "$RemoteUser@$RemoteHost"
$askPassPath = Join-Path $env:TEMP "codex-ssh-askpass-$sessionId.bat"

@"
@echo off
echo $RemotePassword
"@ | Set-Content -LiteralPath $askPassPath -Encoding ASCII

$oldAskPass = $env:SSH_ASKPASS
$oldAskPassRequire = $env:SSH_ASKPASS_REQUIRE
$oldDisplay = $env:DISPLAY

$env:SSH_ASKPASS = $askPassPath
$env:SSH_ASKPASS_REQUIRE = "force"
$env:DISPLAY = "dummy"

try {
    & $sshExePath -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p $RemotePort $sshTarget "mkdir -p '$remoteDir/out'"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to create remote workdir $remoteDir"
    }

    & $scpExePath -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -P $RemotePort -r (Join-Path $projectRoot "src") "$sshTarget`:$remoteDir/"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to upload src/ to $remoteDir"
    }

    & $scpExePath -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -P $RemotePort $inputPath "$sshTarget`:$remoteDir/$remoteInputName"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to upload input file to $remoteDir"
    }

    $runPrefix = if ($DebugProgress) { "SIMPLELANG_DEBUG_PROGRESS=1 " } else { "" }
    $runArgs = if ($ParseOnly) { "--parse-only " } else { "" }
    if ($ProgressOutput) {
        $runArgs += "--progress-file '$remoteProgressPath' "
    }
    $runArgs += "--target '$Target' "
    $runCommand = "$runPrefix./build/src/parser $runArgs'$remoteInputName' '$remoteAsmPath' '$remoteDgmlPath'"
    if ($RemoteRunTimeoutSeconds -gt 0) {
        $runCommand = "timeout $RemoteRunTimeoutSeconds" + "s " + $runCommand
    }

    $remoteCommand = @(
        "cd '$remoteDir'",
        "mkdir -p build/src out",
        "java -cp /usr/local/lib/antlr-3.4-complete.jar org.antlr.Tool -o build/src src/SimpleLang.g",
        "gcc -o build/src/parser src/main.c src/cfg_builder.c src/jvm_backend.c build/src/src/*.c -O0 -g -I/usr/local/include -Isrc -Ibuild/src/src -L/usr/local/lib -lantlr3c",
        $runCommand
    ) -join " && "

    & $sshExePath -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p $RemotePort $sshTarget $remoteCommand
    $remoteExitCode = $LASTEXITCODE

    if ($ProgressOutput) {
        $progressPath = Resolve-AbsolutePath -Path $ProgressOutput
        New-Item -ItemType Directory -Force (Split-Path -Parent $progressPath) | Out-Null
        & $scpExePath -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -P $RemotePort "$sshTarget`:$remoteDir/$remoteProgressPath" $progressPath | Out-Null
    }

    if ($remoteExitCode -ne 0) {
        throw "Remote parser build/run failed"
    }

    & $scpExePath -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -P $RemotePort "$sshTarget`:$remoteDir/$remoteAsmPath" $asmPath
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to download generated assembly"
    }

    if ($Target -eq "jvm") {
        $classOutput = Join-Path (Split-Path -Parent $asmPath) "SimpleLangProgram.class"
        & $scpExePath -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -P $RemotePort "$sshTarget`:$remoteDir/out/SimpleLangProgram.class" $classOutput | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "Failed to download generated JVM class file to $classOutput"
        } else {
            Write-Host "JVM class written to $classOutput"
        }
    }

    $symOutput = "$asmPath.sym"
    if ($Target -ne "jvm") {
        & $scpExePath -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -P $RemotePort "$sshTarget`:$remoteDir/$remoteAsmPath.sym" $symOutput | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "Failed to download generated sym file to $symOutput"
        }
    }

    & $scpExePath -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -P $RemotePort "$sshTarget`:$remoteDir/$remoteDgmlPath" $dgmlPath
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to download parse tree DGML"
    }

    Write-Host "Assembly written to $asmPath"
    if ($Target -ne "jvm" -and (Test-Path -LiteralPath $symOutput)) {
        Write-Host "Symbols written to $symOutput"
    }
    Write-Host "Parse tree written to $dgmlPath"
}
finally {
    $env:SSH_ASKPASS = $oldAskPass
    $env:SSH_ASKPASS_REQUIRE = $oldAskPassRequire
    $env:DISPLAY = $oldDisplay

    if (Test-Path -LiteralPath $askPassPath) {
        Remove-Item -LiteralPath $askPassPath -Force
    }
}
