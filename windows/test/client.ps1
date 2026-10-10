# 이 파일은 UTF-8 BOM 으로 저장한다. Windows PowerShell 5.1 은 BOM 이 없으면 .ps1 을
# 시스템 ANSI 코드페이지(한국어 Windows 는 949)로 읽어 한글 문자열 리터럴이 깨지고,
# 닫는 따옴표가 멀티바이트 문자에 먹혀 파싱까지 실패한다.
#
# Windows 클라이언트 시나리오 테스트. test/client.sh 의 run / idle / restart 를 옮긴 것이다.
# 빌드 -> (필요하면) 권한 상승 -> 실행 -> 시나리오별 PASS / FAIL.
# 서버 장비에서 test/server.sh 가 먼저 떠 있어야 한다.
#
#   windows\test\client.ps1                  일반 실행 (Ctrl-C 로 종료)
#   windows\test\client.ps1 idle [-Sec 120]  방치: 재핸드셰이크가 없어야 하고 세션이 살아 있어야 함
#   windows\test\client.ps1 restart          서버 재시작 복구. 서버는 'test/server.sh restart' 로 실행
#
# 설정은 파라미터나 환경변수로 바꾼다. 예) $env:SERVER_IP='1.2.3.4'; windows\test\client.ps1

[CmdletBinding()]
param(
    [ValidateSet('run', 'idle', 'restart')]
    [string]$Mode = 'run',

    [int]$Sec = 0,

    [string]$ServerIp    = $(if ($env:SERVER_IP)      { $env:SERVER_IP }      else { '49.247.139.39' }),
    [int]   $Port        = $(if ($env:PORT)           { [int]$env:PORT }      else { 9000 }),
    [string]$ServerTunIp = $(if ($env:SERVER_TUN_IP)  { $env:SERVER_TUN_IP }  else { '10.0.0.1' }),
    # Linux 클라이언트가 10.0.0.2 를 쓰므로 Windows 는 10.0.0.3 을 기본으로 한다.
    [string]$ClientTunIp = $(if ($env:CLIENT_TUN_IP)  { $env:CLIENT_TUN_IP }  else { '10.0.0.3' }),
    [string]$Adapter     = $(if ($env:ADAPTER)        { $env:ADAPTER }        else { 'my_vpn' }),
    [string]$Config      = $(if ($env:CONFIG)         { $env:CONFIG }         else { 'Debug' }),

    # 권한 상승 후 자기 자신을 다시 부를 때만 쓴다.
    [switch]$Elevated
)

$ErrorActionPreference = 'Stop'

$TestDir     = $PSScriptRoot
$WindowsRoot = Split-Path -Parent $TestDir
$LogDir      = Join-Path $TestDir 'logs'
$Project     = Join-Path $WindowsRoot 'cli\vpn_client.vcxproj'
$Exe         = Join-Path $WindowsRoot "build\$Config\vpn_client.exe"
$ClientLog   = Join-Path $LogDir 'client.log'
$Report      = Join-Path $LogDir "client-$Mode.txt"

if ($Sec -le 0) {
    $Sec = 120
}

# ---- 공용 ---------------------------------------------------------------
$script:Transcript = New-Object System.Collections.Generic.List[string]

function Say([string]$msg) {
    $line = "[test] $msg"
    Write-Host $line
    $script:Transcript.Add($line)
}

function Save-Transcript {
    New-Item -ItemType Directory -Force $LogDir | Out-Null
    Set-Content -Path $Report -Value $script:Transcript -Encoding utf8
}

function Die([string]$msg) {
    Say "ERROR: $msg"
    Save-Transcript
    exit 1
}

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltinRole]::Administrator)
}

function Find-MSBuild {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        return $null
    }
    return (& $vswhere -latest -requires Microsoft.Component.MSBuild `
        -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1)
}

# 로그 파일에서 문자열 등장 횟수를 센다 (common.sh 의 log_count).
function Get-LogCount([string]$needle) {
    if (-not (Test-Path $ClientLog)) {
        return 0
    }
    $text = Get-Content $ClientLog -Raw -ErrorAction SilentlyContinue
    if (-not $text) {
        return 0
    }
    return @([regex]::Matches($text, [regex]::Escape($needle))).Count
}

# wait_log <문자열> <timeout초> [최소 등장 횟수]
function Wait-Log([string]$needle, [int]$timeout, [int]$want = 1) {
    $end = (Get-Date).AddSeconds($timeout)
    while ((Get-LogCount $needle) -lt $want) {
        if ((Get-Date) -ge $end) {
            return $false
        }
        if ($script:Proc -and $script:Proc.HasExited) {
            return $false
        }
        Start-Sleep -Milliseconds 500
    }
    return $true
}

function Test-Ping([string]$ip) {
    & ping.exe -n 3 -w 2000 $ip > $null 2>&1
    return ($LASTEXITCODE -eq 0)
}

# 여러 번 불리므로 지난번에 보여 준 줄은 다시 찍지 않는다.
$script:IdxShown = 0

function Show-Idx {
    $all = @(Get-Content $ClientLog -ErrorAction SilentlyContinue |
        Select-String -SimpleMatch '[HS] established')

    for ($i = $script:IdxShown; $i -lt $all.Count; $i++) {
        Say "  $($all[$i])"
    }
    $script:IdxShown = $all.Count
}

# 콘솔 클라이언트는 Ctrl-C 로 끝내는 것이 정상 경로다 (SetConsoleCtrlHandler -> stop 이벤트 ->
# tunnel_run 이 어댑터와 터널 IP 를 스스로 정리). 자식이 우리 콘솔을 공유하므로
# GenerateConsoleCtrlEvent(group 0) 로 보낼 수 있는데, 그러면 이 스크립트도 같이 받는다.
# SetConsoleCtrlHandler(NULL, TRUE) 로 우리 쪽만 Ctrl-C 를 무시하게 해 두고 보낸다.
if (-not ('MyVpn.ConsoleCtrl' -as [type])) {
    Add-Type -Namespace MyVpn -Name ConsoleCtrl -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool GenerateConsoleCtrlEvent(uint dwCtrlEvent, uint dwProcessGroupId);

[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool SetConsoleCtrlHandler(IntPtr handlerRoutine, bool add);
'@
}

function Send-CtrlC {
    try {
        if (-not [MyVpn.ConsoleCtrl]::SetConsoleCtrlHandler([IntPtr]::Zero, $true)) {
            return $false
        }
        try {
            return [MyVpn.ConsoleCtrl]::GenerateConsoleCtrlEvent(0, 0)   # 0 = CTRL_C_EVENT
        }
        finally {
            [MyVpn.ConsoleCtrl]::SetConsoleCtrlHandler([IntPtr]::Zero, $false) | Out-Null
        }
    }
    catch {
        return $false
    }
}

function Stop-Client {
    if ($script:Proc -and -not $script:Proc.HasExited) {
        if ((Send-CtrlC) -and $script:Proc.WaitForExit(10000)) {
            Say 'stopped by Ctrl-C (graceful)'
        }
        else {
            # 정상 경로가 안 되면 강제로 끊는다. 이때는 정리 구간이 돌지 않으므로
            # 아래 어댑터 검사 결과를 그대로 신뢰하면 안 된다.
            Say 'Ctrl-C did not stop it; forcing (cleanup path NOT exercised)'
            $script:Proc.Kill()
            $script:Proc.WaitForExit(5000) | Out-Null
        }
    }

    Start-Sleep -Seconds 1
    $left = @(Get-NetAdapter -Name $Adapter -ErrorAction SilentlyContinue).Count
    $ipLeft = @(Get-NetIPAddress -IPAddress $ClientTunIp -ErrorAction SilentlyContinue).Count
    Say "after stop: adapter=$left ip $ClientTunIp=$ipLeft (both 0 = cleaned up)"
}

function Finish([int]$code, [string]$msg) {
    if ($code -eq 0) {
        Say "PASS: $msg"
    }
    else {
        Say "FAIL: $msg"
    }

    # Stop-Client 는 Ctrl-C 를 콘솔 그룹 전체에 보내므로 이 스크립트까지 같이 죽을 수 있다.
    # 판정 결과를 먼저 디스크에 남기고, 살아남으면 정리 결과를 덧붙여 다시 저장한다.
    Save-Transcript
    Stop-Client
    Say "log: $ClientLog"
    Save-Transcript
    exit $code
}

# ---- 빌드 + 권한 상승 ---------------------------------------------------
if (-not $Elevated) {
    $msb = Find-MSBuild
    if (-not $msb) {
        Die 'MSBuild not found (Visual Studio required)'
    }

    Say "build: $Config x64"
    & $msb $Project /p:Configuration=$Config /p:Platform=x64 /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) {
        Die 'build failed'
    }
}

if (-not (Test-Admin)) {
    # 어댑터 생성과 IP / MTU 설정에 관리자 권한이 필요하다.
    # 빌드는 일반 권한으로 끝냈고 여기서만 올린다 (test/common.sh 의 become_root 와 같은 구조).
    Say 'elevating (UAC)'

    # Start-Process 는 -ArgumentList 배열을 공백으로 이어붙이기만 하고 공백이 든 인자를
    # 따로 인용해 주지 않는다. 경로에 공백이 있으면 잘리므로 직접 인용해 한 문자열로 넘긴다.
    $q = { param($v) '"' + ($v -replace '"', '\"') + '"' }
    $argStr = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', (& $q $PSCommandPath),
        '-Mode', $Mode,
        '-Sec', $Sec,
        '-ServerIp', (& $q $ServerIp),
        '-Port', $Port,
        '-ServerTunIp', (& $q $ServerTunIp),
        '-ClientTunIp', (& $q $ClientTunIp),
        '-Adapter', (& $q $Adapter),
        '-Config', (& $q $Config),
        '-Elevated'
    ) -join ' '

    # UAC 를 거절하면 Start-Process 가 예외를 던진다. 테스트 실패와 구분되게 받아 준다.
    try {
        $child = Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $argStr -PassThru -Wait
    }
    catch {
        Die "elevation declined ($($_.Exception.Message.Trim())); 어댑터 생성에 관리자 권한이 필요합니다"
    }

    Write-Host ''
    if (Test-Path $Report) {
        Get-Content $Report
    }
    elseif (Test-Path $ClientLog) {
        # 판정까지 못 간 경우. 최소한 클라이언트가 뭘 했는지는 보여 준다.
        Say "no verdict written; tail of $ClientLog :"
        Get-Content $ClientLog -Tail 15
    }
    exit $child.ExitCode
}

# ---- 실행 ---------------------------------------------------------------
if (-not (Test-Path $Exe)) {
    Die "not built: $Exe"
}

# 이전 실행이 남긴 어댑터가 있으면 터널 IP 가 겹쳐 핸드셰이크가 엉킨다.
if (Get-NetAdapter -Name $Adapter -ErrorAction SilentlyContinue) {
    Die "adapter '$Adapter' already exists; close the other client first"
}

New-Item -ItemType Directory -Force $LogDir | Out-Null
Set-Content -Path $ClientLog -Value '' -Encoding utf8

Say "client: $Adapter $ClientTunIp -> server ${ServerIp}:${Port} (tunnel $ServerTunIp)"

$script:Proc = Start-Process -FilePath $Exe `
    -ArgumentList $Adapter, $ServerIp, "$Port", $ClientTunIp `
    -NoNewWindow -PassThru -RedirectStandardOutput $ClientLog `
    -RedirectStandardError (Join-Path $LogDir 'client.err')

if (-not (Wait-Log 'tun device ready' 15)) {
    Finish 1 'client failed to start'
}

$ipRow = Get-NetIPAddress -InterfaceAlias $Adapter -AddressFamily IPv4 -ErrorAction SilentlyContinue
$ifRow = Get-NetIPInterface -InterfaceAlias $Adapter -AddressFamily IPv4 -ErrorAction SilentlyContinue
Say "$Adapter up: $($ipRow.IPAddress)/$($ipRow.PrefixLength) mtu $($ifRow.NlMtu)"

if (-not (Wait-Log '[HS] established' 10)) {
    Finish 1 "handshake failed: is the server running at ${ServerIp}:${Port}?"
}
Show-Idx

if (-not (Test-Ping $ServerTunIp)) {
    Finish 1 "handshake ok but ping $ServerTunIp failed (server tun ip / ACL?)"
}
Say "ping $ServerTunIp ok"

switch ($Mode) {

'run' {
    Say 'running (Ctrl-C to stop)'
    $script:Proc.WaitForExit()
    Finish 0 'stopped'
}

'idle' {
    $rehsBefore = Get-LogCount 'no rx from server'
    $kaSentBefore = Get-LogCount '[keepalive] sent'
    $kaRecvBefore = Get-LogCount '[keepalive] recv'
    $txBefore = Get-LogCount '[tun->udp]'

    Say "idle ${Sec}s start"
    $end = (Get-Date).AddSeconds($Sec)
    while ((Get-Date) -lt $end -and -not $script:Proc.HasExited) {
        Start-Sleep -Seconds 5
        $left = [int]((New-TimeSpan -End $end).TotalSeconds)
        Write-Host -NoNewline "`r[test] idle: $left s left   "
    }
    Write-Host ''

    $rehs   = (Get-LogCount 'no rx from server') - $rehsBefore
    $kaSent = (Get-LogCount '[keepalive] sent') - $kaSentBefore
    $kaRecv = (Get-LogCount '[keepalive] recv') - $kaRecvBefore
    $tx     = (Get-LogCount '[tun->udp]') - $txBefore
    Say "idle done: keepalive sent=$kaSent recv=$kaRecv, tun->udp=$tx, re-handshake=$rehs"

    if ($rehs -ne 0) {
        Finish 1 're-handshake happened while the server was alive'
    }
    if ($kaRecv -lt 1) {
        Finish 1 'no keepalive reply from the server during idle'
    }
    if (-not (Test-Ping $ServerTunIp)) {
        Finish 1 "session lost after ${Sec}s idle (keepalive not working?)"
    }
    Finish 0 "no re-handshake in ${Sec}s idle and the session is still alive"
}

'restart' {
    $established = Get-LogCount '[HS] established'

    Say 'waiting for the server to be restarted (up to 180s)'
    if (-not (Wait-Log '[HS] established' 180 ($established + 1))) {
        Finish 1 "no re-handshake (did the server run 'test/server.sh restart'?)"
    }
    Show-Idx

    Start-Sleep -Seconds 1
    if (-not (Test-Ping $ServerTunIp)) {
        Finish 1 're-handshake done but ping does not recover'
    }
    Finish 0 'recovered from server restart with a new session'
}

}
