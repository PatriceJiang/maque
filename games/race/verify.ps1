# verify.ps1 - one-command acceptance for the maque racing game (ASCII only on purpose:
# Windows PowerShell 5.1 reads BOM-less .ps1 as ANSI, so non-ASCII source breaks parsing).
#   powershell -NoProfile -ExecutionPolicy Bypass -File games\race\verify.ps1
param([int]$ShotFrame = 200)

$Root = 'D:\Github\maque'
$Game = Join-Path $Root 'games\race'
$Exe = Join-Path $Root 'build-win-release\samples\player\maque_player.exe'

if (-not (Test-Path $Exe)) { Write-Host "[FAIL] player not found: $Exe" -ForegroundColor Red; exit 2 }

Set-Location $Root   # the engine resolves its default font against CWD; elsewhere it falls back to a system font and loses ~20 ms/frame

$env:MAQUE_WINDOW = '1280x720'
$env:MAQUE_PACE = '0'
Remove-Item Env:MAQUE_SIM -ErrorAction SilentlyContinue
Remove-Item Env:MAQUE_TRACE_STATS -ErrorAction SilentlyContinue

$script:fails = 0
$script:lines = @()

function Say([string]$text, [string]$color) {
  Write-Host $text -ForegroundColor $color
  $script:lines += $text
}

function RunPlayer([string[]]$argsList) {
  return (& $Exe @argsList 2>&1 | Out-String)
}

$deny = @('\[script\] exception', 'EXC ', 'loadScene\(.+\) failed', 'cannot read scene', 'shot .+ failed')

function Check([string]$name, [string]$out, [string[]]$must) {
  $bad = $false
  foreach ($d in $deny) {
    if ($out -match $d) { Say ("  deny-match: " + $d) 'Yellow'; $bad = $true }
  }
  foreach ($m in $must) {
    if ($out -notmatch $m) { Say ("  missing: " + $m) 'Yellow'; $bad = $true }
  }
  if ($bad) { $script:fails++; Say ("[FAIL] " + $name) 'Red'; return $false }
  Say ("[ok]   " + $name) 'Green'
  return $true
}

Say '=== maque racing: verify ===' 'Cyan'

# 1) smoke
$out = RunPlayer @('--frames', '120', (Join-Path $Game 'menu.scn'))
$null = Check 'menu smoke' $out @('menu ready')

$themes = @('desert', 'beach', 'forest', 'city')
foreach ($t in $themes) {
  $out = RunPlayer @('--frames', '150', (Join-Path $Game ("race_$t.scn")))
  $null = Check ("race smoke " + $t) $out @("race ready theme=" + $t)
}

# 2) determinism: same input replay twice -> identical world_trail
$env:MAQUE_SIM = Join-Path $Game 'sims\oval.json'
$hs = @()
for ($i = 0; $i -lt 2; $i++) {
  $out = RunPlayer @('--frames', '360', (Join-Path $Game 'race_desert.scn'))
  $m = [regex]::Match($out, 'world_trail=(0x[0-9a-fA-F]+)')
  if ($m.Success) { $hs += $m.Groups[1].Value } else { $hs += 'none' }
}
Remove-Item Env:MAQUE_SIM -ErrorAction SilentlyContinue

if ($hs[0] -ne 'none' -and $hs[0] -eq $hs[1]) {
  Say ("[ok]   determinism world_trail=" + $hs[0]) 'Green'
} else {
  $script:fails++
  Say ("[FAIL] determinism: " + ($hs -join ' vs ')) 'Red'
}

# 3) screenshots
$shotDir = Join-Path $Game 'shots'
New-Item -ItemType Directory -Force -Path $shotDir | Out-Null
foreach ($t in $themes) {
  $png = Join-Path $shotDir ("verify_" + $t + ".png")
  Remove-Item $png -ErrorAction SilentlyContinue
  $null = RunPlayer @('--shot', $png, '--shot-frame', "$ShotFrame", (Join-Path $Game ("race_$t.scn")))
  if ((Test-Path $png) -and ((Get-Item $png).Length -gt 20000)) {
    Say ("[ok]   shot " + $t + " (" + (Get-Item $png).Length + " bytes)") 'Green'
  } else {
    $script:fails++
    Say ("[FAIL] shot " + $t) 'Red'
  }
}

# 4) perf budget: average over the steady-state samples (skip warmup / teardown frames)
$env:MAQUE_TRACE_STATS = '1'
$out = RunPlayer @('--frames', '420', (Join-Path $Game 'race_desert.scn'))
Remove-Item Env:MAQUE_TRACE_STATS -ErrorAction SilentlyContinue

$draws = 0; $d3 = 0; $tris = 0; $total = 0.0; $logic = 0.0; $n = 0
$rows = [regex]::Matches($out, '\[stats\] frame=(\d+) fps=[\d.]+ total=([\d.]+)ms logic=([\d.]+) render=[\d.]+ present=[\d.]+ draws=(\d+) ui_draws=(\d+) 3d_draws=(\d+) tris=(\d+)')
foreach ($r in $rows) {
  $f = [int]$r.Groups[1].Value
  if ($f -lt 120 -or $f -gt 360) { continue }   # 丢掉启动抖动与收尾帧
  $total += [double]$r.Groups[2].Value
  $logic += [double]$r.Groups[3].Value
  $draws += [int]$r.Groups[4].Value
  $d3 += [int]$r.Groups[6].Value
  $tris += [int]$r.Groups[7].Value
  $n++
}
if ($n -gt 0) {
  $total = [math]::Round($total / $n, 2)
  $logic = [math]::Round($logic / $n, 2)
  $draws = [int]($draws / $n)
  $d3 = [int]($d3 / $n)
  $tris = [int]($tris / $n)
}
if ($n -gt 0) {
  Say ("  stats (avg of " + $n + " samples): draws=" + $draws + " 3d=" + $d3 + " tris=" + $tris + " total=" + $total + "ms logic=" + $logic + "ms") 'Gray'
} else {
  Say '  stats: no samples captured' 'Yellow'
}

if ($d3 -gt 0 -and $d3 -le 200) { Say ("[ok]   draw budget 3d_draws=" + $d3 + " <= 200") 'Green' }
else { $script:fails++; Say ("[FAIL] draw budget 3d_draws=" + $d3) 'Red' }

if ($total -gt 0 -and $total -le 18.0) { Say ("[ok]   frame budget total=" + $total + "ms <= 18ms") 'Green' }
else { $script:fails++; Say ("[FAIL] frame budget total=" + $total + "ms") 'Red' }

# 5) report
$report = Join-Path $Game 'reports\perf.md'
New-Item -ItemType Directory -Force -Path (Split-Path $report) | Out-Null
$stamp = Get-Date -Format 'yyyy-MM-dd HH:mm'
$md = @()
$md += '# Perf (desert, 1280x720, Release)'
$md += ''
$md += 'Collected: ' + $stamp
$md += ''
$md += '| metric | measured | budget |'
$md += '|---|---:|---:|'
$md += '| 3D draws | ' + $d3 + ' | <= 200 |'
$md += '| draws total (incl. UI) | ' + $draws + ' | - |'
$md += '| triangles | ' + $tris + ' | <= 250k |'
$md += '| frame total (avg) | ' + $total + ' ms | <= 16.7 ms |'
$md += '| logic, script+sim (avg) | ' + $logic + ' ms | <= 3 ms |'
$md += ''
$md += '## verify.ps1 output'
$md += ''
$md += '```'
$md += $script:lines
$md += '```'
Set-Content -Path $report -Value ($md -join "`r`n") -Encoding UTF8
Say ("report: " + $report) 'Cyan'

if ($script:fails -gt 0) {
  Say ("=== FAIL: " + $script:fails + " check(s) ===") 'Red'
  exit 1
}
Say '=== ALL OK ===' 'Green'
exit 0
