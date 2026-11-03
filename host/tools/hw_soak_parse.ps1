<#
  Stage 7B-HW 板级长跑解析（离线，读 _raw.bin，不碰串口）。
  用法:  powershell -File hw_soak_parse.ps1 -RawPath <.._raw.bin> [-OutTxt <..txt>]
  产出:  OutTxt（默认与 raw 同目录同名 .txt）= SOAK_60MIN.txt 风格的每分钟文本日志
         stdout = 总计摘要（SAMPLE 频率 / STAT / 各错误计数 / SEQ 与 seq8 缺口 / CRC 坏帧）
#>
param(
  [Parameter(Mandatory=$true)][string]$RawPath,
  [string]$OutTxt = ""
)
$ErrorActionPreference = "Stop"
if ($OutTxt -eq "") { $OutTxt = [System.IO.Path]::ChangeExtension($RawPath, ".txt") }
$b = [System.IO.File]::ReadAllBytes($RawPath)
$N = $b.Length

function Get-Crc16([byte[]]$d,[int]$off,[int]$len){
  $crc = 0xFFFF
  for($k=$off; $k -lt $off+$len; $k++){
    $crc = $crc -bxor ([int]$d[$k] -shl 8)
    for($j=0; $j -lt 8; $j++){
      if(($crc -band 0x8000) -ne 0){ $crc = (($crc -shl 1) -bxor 0x1021) -band 0xFFFF }
      else { $crc = ($crc -shl 1) -band 0xFFFF }
    }
  }
  return $crc
}
function U32([byte[]]$d,[int]$o){ return ([uint32]$d[$o]) -bor ([uint32]$d[$o+1] -shl 8) -bor ([uint32]$d[$o+2] -shl 16) -bor ([uint32]$d[$o+3] -shl 24) }
function U16([byte[]]$d,[int]$o){ return ([uint32]$d[$o]) -bor ([uint32]$d[$o+1] -shl 8) }

$cumSample=0; $cumStat=0; $cumEvent=0; $cumAck=0; $cumBoot=0; $cumOther=0
$cumCrcBad=0; $cumSeqGap=0; $cumSeq8Gap=0
$seqPrev=-1; $seq8Prev=-1
$t0=-1; $tLast=-1; $curBucket=-1
$stDrops=0; $stFrame=0; $stAdc=0; $stUart=0; $stRx=0
$stUptime=0; $stSamples=0; $stChg=0; $stThrW=0; $stThrC=0; $stLvl=0; $haveStat=0
$lines = @()
$prevS=0; $prevT=0

# 在 60s 桶边界（及收尾）生成一行 SOAK 风格文本
function Emit-Line([int]$labelSec){
  $ds = $cumSample - $prevS
  $dt = $cumStat   - $prevT
  $script:prevS = $cumSample
  $script:prevT = $cumStat
  $line = "[{0,5}s] SAMPLE {1,-6}(+{2,-5}) STAT {3,-5}(+{4,-4}) | drops={5,-6} frame_err={6,-4} adc_err={7,-4} uart_err={8,-4} rx_err={9,-4} | seq8缺口={10,-4} 整帧丢失={11,-4} 解析错={12}" -f `
          $labelSec, $cumSample, $ds, $cumStat, $dt, $stDrops, $stFrame, $stAdc, $stUart, $stRx, $cumSeq8Gap, $cumSeqGap, $cumCrcBad
  $script:lines += $line
}

$i=0
while($i -lt $N-6){
  if($b[$i] -eq 0xAA -and $b[$i+1] -eq 0x55){
    $type=$b[$i+2]; $len=[int]$b[$i+3]; $seq=[int]$b[$i+4]
    $total=5+$len+2
    if($len -le 40 -and ($i+$total) -le $N){
      if((Get-Crc16 $b ($i+2) (3+$len)) -eq [int](U16 $b ($i+5+$len))){
        $pl=$i+5
        $t=-1
        if($type -eq 0x81){
          $cumSample++; $t=[int](U32 $b $pl)
          $sq8=[int]$b[$pl+9]
          if($seq8Prev -ge 0){ if($sq8 -ne (($seq8Prev+1) -band 0xFF)){ $cumSeq8Gap++ } }
          $seq8Prev=$sq8
        } elseif($type -eq 0x82){ $cumEvent++ }
        elseif($type -eq 0x83){
          $cumStat++; $t=[int](U32 $b $pl); $haveStat=1
          $stUptime=$t; $stSamples=U32 $b ($pl+4); $stDrops=U32 $b ($pl+8); $stAdc=U32 $b ($pl+12)
          $stUart=U32 $b ($pl+16); $stRx=U32 $b ($pl+20); $stFrame=U32 $b ($pl+24)
          $stChg=U32 $b ($pl+28); $stThrW=U16 $b ($pl+32); $stThrC=U16 $b ($pl+34); $stLvl=[int]$b[$pl+36]
        } elseif($type -eq 0x84){ $cumAck++ }
        elseif($type -eq 0x85){ $cumBoot++ }
        else { $cumOther++ }

        if($t -ge 0){
          if($t0 -lt 0){ $t0=$t }
          $tLast=$t
          $bucket=[int][Math]::Floor(($t-$t0)/60000)
          if($curBucket -lt 0){ $curBucket=$bucket }
          while($bucket -gt $curBucket){ $curBucket=$curBucket+1; Emit-Line ($curBucket*60) }
        }
        if($seqPrev -ge 0){ if($seq -ne (($seqPrev+1) -band 0xFF)){ $cumSeqGap++ } }
        $seqPrev=$seq
        $i+=$total; continue
      } else { $cumCrcBad++ }
    }
  }
  $i++
}
$elapsedSec = 0
if($t0 -ge 0 -and $tLast -gt $t0){ $elapsedSec = [int](($tLast-$t0)/1000) }
if($elapsedSec -gt $curBucket*60){ Emit-Line $elapsedSec }

$hdr = "duration=$([int]($elapsedSec/60)) min  原始字节=$N  (由 hw_soak_parse.ps1 从 _raw.bin 生成)"
[System.IO.File]::WriteAllLines($OutTxt, (@($hdr)+$lines), [System.Text.UTF8Encoding]::new($false))

$rate = 0
if($elapsedSec -gt 0){ $rate = $cumSample/$elapsedSec }
"=== parse summary ==="
"rawBytes=$N  outTxt=$OutTxt"
"SAMPLE=$cumSample STAT=$cumStat EVENT=$cumEvent ACK=$cumAck BOOT=$cumBoot OTHER=$cumOther"
"crcBad=$cumCrcBad  seqGap(整帧丢失)=$cumSeqGap  seq8Gap=$cumSeq8Gap"
"deviceTimeSpan=${elapsedSec}s  SAMPLE rate={0:N3} Hz" -f $rate
if($haveStat){ "LAST STAT: uptime=$stUptime samples=$stSamples drops=$stDrops adc=$stAdc uart=$stUart rx=$stRx frm=$stFrame chg=$stChg thr=$stThrW/$stThrC lvl=$stLvl" }
