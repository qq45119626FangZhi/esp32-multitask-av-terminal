# =============================================================================
# 打包 OTA 升级包（默认出 V1.0 / V2.0 两个包）
#
# 用法（任选一种）：
#   powershell -ExecutionPolicy Bypass -File tools\pack_ota.ps1
#   powershell -ExecutionPolicy Bypass -File tools\pack_ota.ps1 -Versions 1.0.0,2.0.0,3.0.0
#
# 产物：<工程根>\OTA\ai_xiaozhi_V1.0.bin、ai_xiaozhi_V2.0.bin
#   - 包就是 build\atguigu_aixiaozhi.bin（app 分区镜像，不是 merge 出来的全片镜像）
#   - 设备端设备详情页（SW2 双击）/ App OTA 列表都会显示镜像里的版本号
#
# 说明：版本号通过 CMakeCache 变量 XZ_OTA_VER 覆盖（见根 CMakeLists.txt），
#       全程不改源文件；最后会再编译一次默认版本，让 build\ 不残留在 V2.0。
# =============================================================================
param(
    [string[]]$Versions = @('1.0.0', '2.0.0'),
    [string]$IdfProfile = 'C:\Espressif\tools\Microsoft.v5.3.4.PowerShell_profile.ps1',
    [string]$DefaultVer = '1.0.0',
    [switch]$KeepLast
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
if (-not $root) { $root = (Get-Location).Path }
$outDir = Join-Path $root 'OTA'
$bin = Join-Path $root 'build\atguigu_aixiaozhi.bin'

if (-not (Test-Path $IdfProfile)) { throw "找不到 IDF 环境脚本: $IdfProfile" }
. $IdfProfile
Set-Location $root
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

foreach ($ver in $Versions) {
    # "2.0.0" -> "V2.0"（详情页/OTA 列表显示的短版本号）
    $short = 'V' + ($ver -split '\.')[0] + '.' + ($ver -split '\.')[1]
    Write-Host ""
    Write-Host "==== 编译 $ver ($short) ====" -ForegroundColor Cyan
    idf.py "-DXZ_OTA_VER=$ver" build
    if ($LASTEXITCODE -ne 0) { throw "编译 $ver 失败（exit=$LASTEXITCODE）" }
    if (-not (Test-Path $bin)) { throw "没找到镜像: $bin" }

    $dst = Join-Path $outDir ("ai_xiaozhi_" + $short + ".bin")
    Copy-Item -Force $bin $dst
    $mb = [math]::Round((Get-Item $dst).Length / 1MB, 2)
    Write-Host "已生成: $dst  ($mb MiB)" -ForegroundColor Green
}

if (-not $KeepLast -and ($Versions[-1] -ne $DefaultVer)) {
    Write-Host ""
    Write-Host "==== 恢复默认版本 $DefaultVer ====" -ForegroundColor Cyan
    idf.py "-DXZ_OTA_VER=$DefaultVer" build
}

Write-Host ""
Write-Host "全部完成，产物在: $outDir" -ForegroundColor Green
Get-ChildItem $outDir -Filter *.bin | Format-Table Name, Length, LastWriteTime -AutoSize
