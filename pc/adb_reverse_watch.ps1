# 头显休眠 / USB 重枚举会清空 adb reverse 规则，这里持续检测并自动重建。
# 用法: .\adb_reverse_watch.ps1 [-Port 8000]
param([int]$Port = 8000)

Write-Host "watching adb reverse tcp:$Port (Ctrl+C 退出)"
while ($true) {
    $list = adb reverse --list 2>$null
    if ($LASTEXITCODE -ne 0 -or ($list -notmatch "tcp:$Port")) {
        adb wait-for-device
        adb reverse tcp:$Port tcp:$Port | Out-Null
        Write-Host "$(Get-Date -Format HH:mm:ss) restored reverse tcp:$Port"
    }
    Start-Sleep -Seconds 2
}
