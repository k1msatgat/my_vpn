# 이 파일은 UTF-8 BOM 으로 저장한다 (client.ps1 의 머리말과 같은 이유).
#
# Windows GUI 의 사용자 로그인(SSO) 시나리오.
#
# 시험용 IdP(tools/idp/mock_oidc.py)를 이 PC 의 127.0.0.1 에 띄우고, GUI 의 로그인 버튼을 메시지로
# 눌러(WM_COMMAND, 창을 앞으로 가져오지 않는다) 서버 ACL 의 사용자 규칙이 열리고 닫히는지를
# 터널 너머의 포트로 확인한다. 브라우저는 실제로 열리고, IdP 가 바로 앱으로 돌려보낸다
# (--auto-approve).
#
# 준비
#   - 서비스가 설치되어 연결된 상태 (vpn_service install, GUI 에서 한 번 연결).
#   - tools\idp\mint.py keys <KeysDir>   시험용 서명 키와 jwks.json
#   - 서버: vpn_server -u idp.conf 로 띄운다. idp.conf 의 issuer 는 http://127.0.0.1:<IdpPort>
#     (서버는 IdP 에 접속하지 않으므로 이 PC 의 주소여도 된다), jwks 는 위의 jwks.json.
#     acl.conf 에 "... <서버 터널 IP> tcp <UserPort> allow group=ops", 그 포트에서 듣는 서비스
#     (tools/intranet/intranet.py -p <UserPort>).
#
#   windows\test\sso_e2e.ps1 -KeysDir C:\path\to\keys
#
# 시나리오
#   1 로그인 전          사용자 규칙의 포트는 막히고 기기 규칙의 포트는 열린다
#   2 브라우저 로그인     인가 코드 + PKCE 로 받은 토큰이 서버에 닿아 포트가 열린다
#   3 자동 갱신          토큰 수명의 3/4 에 브라우저 없이 새 토큰을 받고 포트가 계속 열려 있다
#   4 GUI 종료           서비스가 토큰을 버려 포트가 닫힌다
#   5 다시 켜기          저장한 refresh token 으로 브라우저 없이 다시 열린다
#   6 로그아웃           포트가 닫히고 IdP 에 refresh token 폐기를 요청한다
#   7 갱신 중 로그아웃    IdP 가 갱신에 늦게 답하는 동안에도 로그아웃이 바로 된다
#   8 시계 어긋남        서버가 새 토큰도 만료라고 하면 갱신을 한 번만 하고 멈춘다

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$KeysDir,
    [int]$IdpPort = 18443,
    [string]$ServerTunIp = '10.0.0.1',
    [int]$UserPort = 8082,
    [int]$DevicePort = 8080,
    [string]$Config = 'Release',
    # 시험용 IdP 가 토큰에 서명할 때 openssl 명령을 쓴다. PATH 에 없으면 Git for Windows 의 것을 쓴다.
    [string]$OpenSslDir = 'C:\Program Files\Git\mingw64\bin'
)

$ErrorActionPreference = 'Stop'

$Root    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Gui     = Join-Path $Root "windows\build\$Config\vpn_gui.exe"
$Mock    = Join-Path $Root 'tools\idp\mock_oidc.py'
$Issuer  = "http://127.0.0.1:$IdpPort"
$RegPath = 'HKCU:\Software\ztgate\vpn_gui\login'
$LogDir  = Join-Path $PSScriptRoot 'logs'
$MockLog = Join-Path $LogDir 'sso_mock_idp.log'
$IdcLogin = 1018

New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

if (-not (Get-Command openssl -ErrorAction SilentlyContinue)) {
    if (-not (Test-Path (Join-Path $OpenSslDir 'openssl.exe'))) { throw "openssl not found (-OpenSslDir)" }
    $env:PATH = "$OpenSslDir;$env:PATH"
}

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class SsoWin {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@

$script:results = @()

function Record([string]$name, [bool]$ok, [string]$detail) {
    $script:results += [pscustomobject]@{ Name = $name; Ok = $ok; Detail = $detail }
    Write-Host ("{0} {1} - {2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name, $detail)
}

function Open([int]$port) {
    try {
        return (Invoke-WebRequest -UseBasicParsing -TimeoutSec 4 "http://${ServerTunIp}:$port/whoami").StatusCode -eq 200
    }
    catch {
        return $false
    }
}

# 조건이 맞을 때까지 기다린다. 걸린 초를 돌려주고, 시간 안에 맞지 않으면 -1.
function WaitFor([scriptblock]$cond, [int]$timeout) {
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $timeout) {
        if (& $cond) { return [int]((Get-Date) - $t0).TotalSeconds }
        Start-Sleep -Milliseconds 500
    }
    return -1
}

function MockCount([string]$pattern) {
    if (-not (Test-Path $MockLog)) { return 0 }
    return @(Select-String -Path $MockLog -SimpleMatch -Pattern $pattern).Count
}

function StopMock {
    Get-NetTCPConnection -LocalPort $IdpPort -State Listen -ErrorAction SilentlyContinue |
        ForEach-Object { Stop-Process -Id $_.OwningProcess -Force -ErrorAction SilentlyContinue }
}

function StartMock([string[]]$extra) {
    StopMock
    Start-Sleep -Milliseconds 500
    $pyArgs = @('-u', $Mock, $KeysDir, $IdpPort, 'alice', '--groups', 'ops', '--issuer', $Issuer, '--auto-approve') + $extra
    Start-Process -FilePath 'python' -ArgumentList $pyArgs -RedirectStandardOutput $MockLog `
        -RedirectStandardError "$MockLog.err" -WindowStyle Hidden | Out-Null
    if ((WaitFor { try { (Invoke-WebRequest -UseBasicParsing -TimeoutSec 2 "$Issuer/.well-known/openid-configuration").StatusCode -eq 200 } catch { $false } } 10) -lt 0) {
        throw "the test IdP did not start (see $MockLog.err)"
    }
}

function StopGui {
    Stop-Process -Name vpn_gui -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
}

function StartGui {
    Start-Process -FilePath $Gui | Out-Null
    if ((WaitFor { (MainWindow) -ne [IntPtr]::Zero } 10) -lt 0) { throw 'the GUI window did not appear' }
}

function MainWindow {
    $ids = @(Get-Process vpn_gui -ErrorAction SilentlyContinue | ForEach-Object { [uint32]$_.Id })
    $script:found = [IntPtr]::Zero
    [SsoWin]::EnumWindows({ param($h, $l)
        $p = 0; [SsoWin]::GetWindowThreadProcessId($h, [ref]$p) | Out-Null
        $c = New-Object System.Text.StringBuilder 64; [SsoWin]::GetClassName($h, $c, 64) | Out-Null
        $t = New-Object System.Text.StringBuilder 64; [SsoWin]::GetWindowText($h, $t, 64) | Out-Null
        if ($ids -contains $p -and $c.ToString() -eq '#32770' -and $t.ToString() -eq 'ztgate') { $script:found = $h; return $false }
        return $true }, [IntPtr]::Zero) | Out-Null
    return $script:found
}

function LoginButtonText {
    $b = [SsoWin]::GetDlgItem((MainWindow), $IdcLogin)
    $t = New-Object System.Text.StringBuilder 64
    [SsoWin]::GetWindowText($b, $t, 64) | Out-Null
    return $t.ToString()
}

# 로그인 버튼을 누른 것과 같은 WM_COMMAND(BN_CLICKED)를 보낸다.
function ClickLogin {
    $w = MainWindow
    $b = [SsoWin]::GetDlgItem($w, $IdcLogin)
    [SsoWin]::SendMessage($w, 0x111, [IntPtr]$IdcLogin, $b) | Out-Null
}

# ---- 준비: GUI 의 로그인 설정을 이 시험의 IdP 로. 끝나면 되돌린다 ------------------------------
$saved = Get-ItemProperty -Path $RegPath -ErrorAction SilentlyContinue
StopGui
New-Item -Path $RegPath -Force | Out-Null
Set-ItemProperty -Path $RegPath -Name issuer -Value $Issuer
Set-ItemProperty -Path $RegPath -Name client_id -Value 'ztgate'
Set-ItemProperty -Path $RegPath -Name scope -Value 'openid'
Remove-ItemProperty -Path $RegPath -Name refresh -ErrorAction SilentlyContinue

try {
    # 수명 40초: 30초에 갱신한다.
    StartMock @('--ttl', '40')
    StartGui
    Start-Sleep -Seconds 3

    # 1
    $u = Open $UserPort; $d = Open $DevicePort
    Record '1 로그인 전' ((-not $u) -and $d) "사용자 포트 $(if ($u) {'열림'} else {'막힘'}), 기기 포트 $(if ($d) {'열림'} else {'막힘'})"

    # 2
    ClickLogin
    $t = WaitFor { Open $UserPort } 25
    Record '2 브라우저 로그인' ($t -ge 0) $(if ($t -ge 0) { "${t}초 뒤 열림, 버튼 '$(LoginButtonText)'" } else { "25초 안에 열리지 않음, 버튼 '$(LoginButtonText)'" })

    # 3
    $before = MockCount 'POST /token'
    $t = WaitFor { (MockCount 'POST /token') -gt $before } 45
    Start-Sleep -Seconds 3
    $u = Open $UserPort
    Record '3 자동 갱신' (($t -ge 0) -and $u) "$(if ($t -ge 0) {"${t}초 뒤 갱신"} else {'갱신 없음'}), 그 뒤 사용자 포트 $(if ($u) {'열림'} else {'막힘'})"

    # 4
    StopGui
    $t = WaitFor { -not (Open $UserPort) } 15
    Record '4 GUI 종료' ($t -ge 0) $(if ($t -ge 0) { "${t}초 안에 닫힘" } else { '닫히지 않음' })

    # 5
    $authz = MockCount 'GET /authorize'
    StartGui
    $t = WaitFor { Open $UserPort } 25
    $again = (MockCount 'GET /authorize') - $authz
    Record '5 다시 켜기' (($t -ge 0) -and $again -eq 0) "$(if ($t -ge 0) {"${t}초 뒤 열림"} else {'열리지 않음'}), 브라우저 로그인 $again 번"

    # 6
    ClickLogin
    $t = WaitFor { -not (Open $UserPort) } 15
    $revoked = WaitFor { (MockCount 'revoked a live refresh token') -ge 1 } 10
    $stored = $null -ne (Get-ItemProperty -Path $RegPath -Name refresh -ErrorAction SilentlyContinue)
    Record '6 로그아웃' (($t -ge 0) -and $revoked -ge 0 -and -not $stored) "$(if ($t -ge 0) {"${t}초 안에 닫힘"} else {'닫히지 않음'}), IdP 폐기 $(if ($revoked -ge 0) {'됨'} else {'안 됨'}), 저장된 refresh token $(if ($stored) {'남음'} else {'없음'})"

    # 7: 수명 24초(18초에 갱신), IdP 는 갱신에 20초 늦게 답한다.
    StopGui
    StartMock @('--ttl', '24', '--refresh-delay', '20')
    StartGui
    Start-Sleep -Seconds 2
    ClickLogin
    $null = WaitFor { Open $UserPort } 25
    $held = WaitFor { (MockCount 'holding a refresh') -ge 1 } 30
    ClickLogin
    $t = WaitFor { -not (Open $UserPort) } 10
    Record '7 갱신 중 로그아웃' (($held -ge 0) -and ($t -ge 0)) "$(if ($held -ge 0) {'갱신 대기 중에'} else {'갱신이 시작되지 않음,'}) 로그아웃, $(if ($t -ge 0) {"${t}초 안에 닫힘"} else {'닫히지 않음'})"

    # 8: 서버 시계보다 100초 앞서 끝난 토큰 (서버의 여유는 30초).
    StopGui
    StartMock @('--ttl', '60', '--exp-offset', '-100')
    Remove-ItemProperty -Path $RegPath -Name refresh -ErrorAction SilentlyContinue
    StartGui
    Start-Sleep -Seconds 2
    ClickLogin
    Start-Sleep -Seconds 15
    $n1 = MockCount 'POST /token'
    Start-Sleep -Seconds 10
    $n2 = MockCount 'POST /token'
    $u = Open $UserPort
    Record '8 시계 어긋남' (($n1 -eq 2) -and ($n2 -eq 2) -and -not $u) "토큰 요청 $n1 번 (10초 뒤 $n2 번), 사용자 포트 $(if ($u) {'열림'} else {'막힘'})"
    if ((LoginButtonText) -eq '로그아웃') { ClickLogin }
}
finally {
    StopMock
    StopGui
    Remove-ItemProperty -Path $RegPath -Name refresh -ErrorAction SilentlyContinue
    foreach ($n in 'issuer', 'client_id', 'scope') {
        if ($saved -and ($saved.PSObject.Properties.Name -contains $n)) {
            Set-ItemProperty -Path $RegPath -Name $n -Value $saved.$n
        }
        else {
            Remove-ItemProperty -Path $RegPath -Name $n -ErrorAction SilentlyContinue
        }
    }
}

$pass = @($script:results | Where-Object { $_.Ok }).Count
Write-Host ''
Write-Host "결과: $pass/$($script:results.Count) 통과"
if ($pass -ne $script:results.Count) { exit 1 }
