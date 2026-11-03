<#
  Stage 7B-HW board soak CAPTURE (read-only serial; does not touch firmware/protocol).
  Usage:  powershell -File hw_soak_capture.ps1 [-Port COM6] [-Seconds 600] [-OutDir dir] [-RunTag tag]
  Output: <OutDir>\soak_<RunTag>_raw.bin        raw bytes (evidence, do not edit)
          <OutDir>\soak_<RunTag>_progress.txt   per-minute progress + final text STAT
  NOTE:   RunTag defaults to a timestamp so repeated runs never overwrite earlier evidence.
          COM6 is exclusive: a second concurrent capture fails at Open() instead of corrupting data.
  Keep this file ASCII-only: Windows PowerShell 5.1 reads BOM-less .ps1 as ANSI and would
  mangle non-ASCII comments.
#>
param(
  [string]$Port = "COM6",
  [int]$Seconds = 600,
  [string]$OutDir = "",
  [string]$RunTag = (Get-Date -Format "yyyyMMdd_HHmmss")
)
$ErrorActionPreference = "Stop"
if ($OutDir -eq "") { $OutDir = Join-Path (Split-Path -Parent $PSScriptRoot) "..\docs" }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
$rawPath  = Join-Path $OutDir ("soak_" + $RunTag + "_raw.bin")
$progPath = Join-Path $OutDir ("soak_" + $RunTag + "_progress.txt")

$p = New-Object System.IO.Ports.SerialPort $Port,115200,"None",8,"One"
$p.ReadTimeout = 500
$p.Open()
Start-Sleep -Milliseconds 200
$p.DiscardInBuffer()

# Zero device counters, then discard the reset-triggered STAT/text so capture starts from a clean baseline.
$p.Write("reset`n")
Start-Sleep -Milliseconds 400
$p.DiscardInBuffer()

$fs = [System.IO.File]::Create($rawPath)
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$buf = New-Object byte[] 8192
$lastMin = 0
$startWall = Get-Date
"start=$($startWall.ToString('s')) port=$Port dur=${Seconds}s" | Out-File $progPath -Encoding utf8

while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
  try { $n = $p.Read($buf,0,$buf.Length) } catch { $n = 0 }
  if ($n -gt 0) { $fs.Write($buf,0,$n) }
  $min = [int][Math]::Floor($sw.Elapsed.TotalSeconds / 60)
  if ($min -gt $lastMin) {
    $lastMin = $min
    "$($min)min elapsed=$([int]$sw.Elapsed.TotalSeconds)s bytes=$($fs.Length)" | Out-File $progPath -Append -Encoding utf8
  }
}
$fs.Flush(); $fs.Close()

$p.Write("stat`n")
Start-Sleep -Milliseconds 600
$tb = New-Object byte[] 4096
$tms = New-Object System.IO.MemoryStream
$tsw = [System.Diagnostics.Stopwatch]::StartNew()
while ($tsw.Elapsed.TotalSeconds -lt 0.8) {
  try { $n = $p.Read($tb,0,$tb.Length); if ($n -gt 0) { $tms.Write($tb,0,$n) } } catch {}
}
$p.Close()
$txt = -join ($tms.ToArray() | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) {[char]$_} elseif ($_ -eq 10) {"`n"} else {""} })

"end=$((Get-Date).ToString('s')) rawBytes=$((Get-Item $rawPath).Length)" | Out-File $progPath -Append -Encoding utf8
"FINAL_TEXT:" | Out-File $progPath -Append -Encoding utf8
$txt | Out-File $progPath -Append -Encoding utf8
"DONE" | Out-File $progPath -Append -Encoding utf8
Write-Host "capture done -> $rawPath"
