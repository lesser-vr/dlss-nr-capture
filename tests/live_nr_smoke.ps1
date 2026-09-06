param(
 [Parameter(Mandatory=$true)][string]$AppPath,
 [switch]$GpuCapture,
 [switch]$ExpectCpu,
 [switch]$Transitions,
 [switch]$CreativeTransitions,
 [switch]$TwoPassTransitions,
 [ValidateSet(1,2,3)][int]$NrPasses=1,
 [ValidateSet(50,75,100)][int]$NrScale=100,
 [string]$DeviceName='',
 [string]$AudioDeviceName='',
 [ValidateSet('NV12','P010','RGB24','RGB32','ARGB32','MJPG','YUY2','UYVY')][string]$Format='NV12',
 [ValidateRange(16,8192)][int]$Width=1920,
 [ValidateRange(16,8192)][int]$Height=1080,
 [ValidateRange(1,240)][int]$Fps=60,
 [ValidateRange(5,600)][int]$Seconds = 15
)
$ErrorActionPreference='Stop'
if (@(Get-Process dlss-nr-capture -ErrorAction SilentlyContinue).Count) { throw 'Close the capture app before this hardware test.' }
$AppPath=(Resolve-Path -LiteralPath $AppPath).Path
$keyRelative='Software\DlssNrCapture\Tests\LiveNR-'+[guid]::NewGuid().ToString('N')
$keyPs='HKCU:\'+$keyRelative
$priorKey=$env:DLSS_NR_TEST_SETTINGS_KEY
$priorGpu=$env:DLSS_NR_GPU_CAPTURE
$app=$null
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class LiveNrProbe {
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c,string n);
 public static IntPtr CaptureWindow(){return FindWindow("DlssNrCaptureWindow",null);}
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h,StringBuilder b,int n);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] static extern IntPtr SendMessageTimeout(IntPtr h,uint m,IntPtr w,IntPtr l,uint flags,uint timeout,out UIntPtr result);
 public static long State(IntPtr h){UIntPtr result;if(SendMessageTimeout(h,0x802E,IntPtr.Zero,IntPtr.Zero,2,1000,out result)==IntPtr.Zero)throw new Exception("State query timed out");return (long)result.ToUInt64();}
 public static long Memory(IntPtr h,int field){UIntPtr result;if(SendMessageTimeout(h,0x8030,(IntPtr)field,IntPtr.Zero,2,1000,out result)==IntPtr.Zero)throw new Exception("Memory query timed out");return (long)result.ToUInt64();}
 [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr h);
 [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr h,int p);
 [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr h);
 [DllImport("user32.dll")] public static extern uint GetMenuItemID(IntPtr h,int p);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetMenuString(IntPtr h,uint p,StringBuilder b,int n,uint f);
}
"@
function Read-Title {
 $app.Refresh(); if($app.HasExited){throw 'Test app exited during transition'}
 $b=[Text.StringBuilder]::new(4096);[void][LiveNrProbe]::GetWindowText($window,$b,4096);$b.ToString()
}
function Post-Command([uint32]$command) {
 if(-not [LiveNrProbe]::PostMessage($window,0x111,[IntPtr]$command,[IntPtr]::Zero)){throw 'Could not post menu command'}
}
function Key([int]$key,[bool]$down=$true) { [void][LiveNrProbe]::PostMessage($window, $(if($down){0x100}else{0x101}),[IntPtr]$key,[IntPtr]::Zero) }
function Tap([int]$key) { Key $key; Key $key $false }
function Wait-State([string]$name,[int]$mask,[int]$expected) {
 $wait=[Diagnostics.Stopwatch]::StartNew()
 while($wait.Elapsed.TotalSeconds -lt 20){$null=Read-Title;$state=[LiveNrProbe]::State($window);if(($state -band $mask) -eq $expected){Write-Host "$name passed (state $state)";return};Start-Sleep -Milliseconds 100}
 throw "$name failed; last state $state"
}
function Select-Menu([int]$position,[string]$label) {
 $m=[LiveNrProbe]::GetSubMenu([LiveNrProbe]::GetMenu($window),$position)
 for($j=0;$j -lt [LiveNrProbe]::GetMenuItemCount($m);$j++) {
  $b=[Text.StringBuilder]::new(128);[void][LiveNrProbe]::GetMenuString($m,[uint32]$j,$b,128,0x400)
  if($b.ToString() -eq $label){Post-Command ([LiveNrProbe]::GetMenuItemID($m,$j));return}
 }
 throw "Required hardware mode unavailable: $label"
}
function Wait-Path([string]$name,[string]$pattern,[hashtable]$settings) {
 $first=Read-Title
 $floor=if($first -match 'capture GPU/CPU (\d+)/(\d+)'){[long]$Matches[1]+[long]$Matches[2]}else{0}
 $timer=[Diagnostics.Stopwatch]::StartNew()
 while($timer.Elapsed.TotalSeconds -lt 20) {
  Start-Sleep -Milliseconds 250
  $t=Read-Title
  $actual=Get-ItemProperty -LiteralPath $keyPs
  $matched=$true
  foreach($entry in $settings.GetEnumerator()){if($actual.($entry.Key) -ne $entry.Value){$matched=$false}}
  if($matched -and $t -match $pattern -and $t -match 'capture GPU/CPU (\d+)/(\d+)' -and
      ([long]$Matches[1]+[long]$Matches[2]) -gt $floor+30 -and $t -match 'correction active') {
   Write-Host "$name passed: $t";return
  }
 }
 throw "Transition failed: $name; last status: $t"
}
try {
 $env:DLSS_NR_TEST_SETTINGS_KEY=$keyRelative
 $env:DLSS_NR_GPU_CAPTURE=if($GpuCapture){'1'}else{'0'}
 New-Item -Path $keyPs -Force | Out-Null
 if($DeviceName){New-ItemProperty -LiteralPath $keyPs -Name VideoDevice -Value $DeviceName -PropertyType String | Out-Null}
 if($AudioDeviceName){
  New-ItemProperty -LiteralPath $keyPs -Name AudioDevice -Value $AudioDeviceName -PropertyType String | Out-Null
  New-ItemProperty -LiteralPath $keyPs -Name AudioAutoSync -Value 1 -PropertyType DWord | Out-Null
 }
 foreach($item in @{NrEnabled=1;NrTemporal=1;NrPasses=$NrPasses;NrScale=$NrScale;Width=$Width;Height=$Height;FpsNumerator=$Fps;FpsDenominator=1;FlipVertical=0}.GetEnumerator()) {
  New-ItemProperty -LiteralPath $keyPs -Name $item.Key -Value $item.Value -PropertyType DWord | Out-Null
 }
 New-ItemProperty -LiteralPath $keyPs -Name VideoFormat -Value $Format -PropertyType String | Out-Null
 $app=Start-Process -FilePath $AppPath -WorkingDirectory (Split-Path $AppPath) -WindowStyle Hidden -PassThru
 $clock=[Diagnostics.Stopwatch]::StartNew()
 $title='';$window=[IntPtr]::Zero
 while($clock.Elapsed.TotalSeconds -lt $Seconds) {
  Start-Sleep -Milliseconds 250
  $app.Refresh()
  if($app.HasExited){throw "Capture exited: $($app.ExitCode)"}
  $window=[LiveNrProbe]::CaptureWindow()
  if($window -ne [IntPtr]::Zero){
   [uint32]$owner=0
   [void][LiveNrProbe]::GetWindowThreadProcessId($window,[ref]$owner)
   if($owner -ne $app.Id){throw 'Capture window belongs to another process; refusing to interact.'}
   $text=[Text.StringBuilder]::new(4096)
   [void][LiveNrProbe]::GetWindowText($window,$text,4096)
   $title=$text.ToString()
  }
 }
 if($title -notmatch '[1-9][0-9]* corrections, [1-9][0-9]* enhanced'){throw "NR did not produce/display corrections: $title"}
 $actual=Get-ItemProperty -LiteralPath $keyPs
 if($actual.Width -ne $Width -or $actual.Height -ne $Height -or $actual.VideoFormat -ne $Format -or
    $actual.FpsNumerator -ne $Fps*$actual.FpsDenominator){throw "Requested capture mode was not selected: $title"}
 if($ExpectCpu -and $title -notmatch 'capture GPU/CPU 0/[1-9][0-9]*'){throw "CPU fallback not exercised: $title"}
 if($GpuCapture -and -not $ExpectCpu -and $title -notmatch 'capture GPU/CPU [1-9][0-9]*/0\b'){throw "Exclusive native GPU capture not exercised: $title"}
 if($Transitions) {
  if(-not $GpuCapture -or $ExpectCpu){throw 'Transitions require native GPU capture at startup'}
  Select-Menu 3 '30.00 fps'; Wait-Path '30 fps' 'GPU native' @{FpsNumerator=30;FpsDenominator=1}
  Select-Menu 3 '60.00 fps'; Wait-Path '60 fps' 'GPU native' @{FpsNumerator=60;FpsDenominator=1}
  Select-Menu 2 '2560x1440'; Wait-Path '1440p' 'GPU native' @{Width=2560;Height=1440}
  Select-Menu 2 '1920x1080'; Wait-Path '1080p' 'GPU native' @{Width=1920;Height=1080}
  Select-Menu 1 'P010'; Wait-Path 'P010' 'GPU native' @{VideoFormat='P010'}
  Select-Menu 1 'RGB24'; Wait-Path 'RGB24 automatic flip' 'CPU: driver supplied system-memory sample' @{VideoFormat='RGB24';FlipVertical=1}
  Post-Command 44000; Wait-Path 'RGB24 CPU fallback without flip' 'CPU: driver supplied system-memory sample' @{FlipVertical=0}
  Select-Menu 1 'MJPG'; Wait-Path 'MJPG CPU fallback' 'CPU: driver supplied system-memory sample' @{VideoFormat='MJPG'}
  Select-Menu 1 'NV12'; Wait-Path 'NV12 GPU return' 'GPU native' @{VideoFormat='NV12'}
  Post-Command 44000; Wait-Path 'GPU flip' 'GPU native' @{FlipVertical=1}
  Post-Command 44000; Wait-Path 'flip off GPU return' 'GPU native' @{FlipVertical=0}
  Post-Command 50002; Wait-Path 'history overlay CPU fallback' 'CPU: history overlay enabled' @{}
  Post-Command 50002; Wait-Path 'history overlay off GPU return' 'GPU native' @{}
  Post-Command 50003; Wait-Path 'GPU capture disabled' 'CPU: GPU capture disabled' @{GpuNativeCapture=0}
  Post-Command 50003; Wait-Path 'GPU capture enabled' 'GPU native' @{GpuNativeCapture=1}
  [void][LiveNrProbe]::PostMessage($window,0x802A,[IntPtr]::Zero,[IntPtr]::Zero)
  Wait-Path 'capture error recovery' 'capture reconnects [1-9][0-9]*' @{VideoFormat='NV12';Width=1920;Height=1080;FlipVertical=0}
  if(-not $AudioDeviceName){Post-Command 48020}
  Wait-Path 'automatic AV option' 'GPU native' @{AudioAutoSync=1}
  [void][LiveNrProbe]::PostMessage($window,0x802D,[IntPtr]::Zero,[IntPtr]::Zero)
  Wait-Path 'graphics recreation' 'graphics recoveries 1' @{VideoFormat='NV12';Width=1920;Height=1080;AudioAutoSync=1}
  # Application-only power messages; does not suspend Windows or touch the console.
  [void][LiveNrProbe]::PostMessage($window,0x218,[IntPtr]4,[IntPtr]::Zero)
  Start-Sleep -Milliseconds 500
  [void][LiveNrProbe]::PostMessage($window,0x218,[IntPtr]18,[IntPtr]::Zero)
  Wait-Path 'application resume' 'graphics recoveries 2' @{VideoFormat='NV12';Width=1920;Height=1080;AudioAutoSync=1}
  $title=Read-Title
 }
 if($TwoPassTransitions) {
  Post-Command 51543;Wait-Path 'two-pass start' 'NR passes 2' @{NrPasses=2}
  foreach($cycle in 1..3) {
   Post-Command 51542;Wait-Path "one-pass cycle $cycle" 'NR passes 1' @{NrPasses=1}
   Post-Command 51543;Wait-Path "two-pass cycle $cycle" 'NR passes 2' @{NrPasses=2}
  }
  Tap 120;Wait-State 'paired two-pass output' 49 49
  Tap 119;Wait-State 'hold two-pass pair' 8 8
  Post-Command 51542;Wait-State 'pass change clears held pair' 8 0
  Wait-Path 'one-pass pair recovers' 'NR passes 1' @{NrPasses=1}
  Tap 120;Wait-State 'return to live view' 1 0
  Tap 121;Wait-State 'NR OFF original fallback' 32 0
  Post-Command 51543;Start-Sleep -Milliseconds 300
  if((Get-ItemProperty -LiteralPath $keyPs).NrEnabled -ne 0){throw 'Pass selection enabled NR while OFF'}
  Wait-State 'OFF pass selection remains original' 32 0
  Tap 121;Wait-Path 'two-pass ON recovery' 'NR passes 2' @{NrPasses=2;NrEnabled=1}
  Select-Menu 3 '30.00 fps';Wait-Path 'two-pass 30 fps' 'NR passes 2' @{NrPasses=2;FpsNumerator=30;FpsDenominator=1}
  Select-Menu 3 '60.00 fps';Wait-Path 'two-pass 60 fps' 'NR passes 2' @{NrPasses=2;FpsNumerator=60;FpsDenominator=1}
  Select-Menu 2 '2560x1440';Wait-Path 'two-pass 1440p' 'NR passes 2' @{NrPasses=2;Width=2560;Height=1440}
  Select-Menu 2 '1920x1080';Wait-Path 'two-pass 1080p' 'NR passes 2' @{NrPasses=2;Width=1920;Height=1080}
  [void][LiveNrProbe]::PostMessage($window,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
  if(-not $app.WaitForExit(5000) -or $app.ExitCode -ne 0){throw 'Two-pass shutdown failed'}
  $app=Start-Process -FilePath $AppPath -WorkingDirectory (Split-Path $AppPath) -WindowStyle Hidden -PassThru
  $restart=[Diagnostics.Stopwatch]::StartNew()
  do {
   Start-Sleep -Milliseconds 100;$window=[LiveNrProbe]::CaptureWindow()
   [uint32]$owner=0
   if($window -ne [IntPtr]::Zero){[void][LiveNrProbe]::GetWindowThreadProcessId($window,[ref]$owner)}
  } while($owner -ne $app.Id -and $restart.Elapsed.TotalSeconds -lt 10)
  if($owner -ne $app.Id){throw 'Restarted test window not found'}
  Wait-Path 'two-pass restart restores live NR' 'NR passes 2' @{NrPasses=2;NrEnabled=1;Width=1920;Height=1080}
  $usage=[LiveNrProbe]::Memory($window,1);$budget=[LiveNrProbe]::Memory($window,2)
  if($usage -le 0 -or $budget -le 0){throw 'Worker DXGI memory telemetry unavailable on required hardware test'}
  Write-Host "Worker local memory after restart: $usage MiB / $budget MiB budget; possible pressure=$([LiveNrProbe]::Memory($window,0))"
  $title=Read-Title
 }
 if($CreativeTransitions) {
  Post-Command 51543;Wait-Path 'two NR passes' 'NR passes 2' @{NrPasses=2}
  Post-Command 51523;Wait-Path 'two passes at 75 percent' 'NR passes 2' @{NrPasses=2;NrScale=75}
  Post-Command 51542;Wait-Path 'return to one pass' 'NR passes 1' @{NrPasses=1}
  Post-Command 51541;Wait-Path 'pass defaults' 'NR passes 1' @{NrPasses=1;NrScale=100}
  Post-Command 51502;Wait-Path 'Tone 50' 'GPU native' @{NrTone=50}
  Post-Command 51512;Wait-Path 'Structure 50' 'GPU native' @{NrStructure=50}
  Post-Command 51523;Wait-Path 'NR 75 percent' 'GPU native' @{NrScale=75}
  Post-Command 51522;Wait-Path 'NR 50 percent' 'GPU native' @{NrScale=50}
  Post-Command 51534;Wait-Path 'preserve color 100' 'GPU native' @{NrColorPreserve=100}
  Post-Command 51540;Wait-Path 'protect highlights' 'GPU native' @{NrHighlightGuard=1}
  Post-Command 51541;Wait-Path 'creative defaults' 'GPU native' @{NrTone=100;NrStructure=100;NrScale=100;NrColorPreserve=0;NrHighlightGuard=0}
  Key 9;Wait-State 'normal view Tab original peek without comparison' 3 2
  Key 9 $false;Wait-State 'normal view Tab release' 3 0
  Tap 120;Wait-State 'F9 paired comparison' 49 49
  Key 9;Wait-State 'Tab original peek' 2 2
  Key 9 $false;Wait-State 'Tab release' 2 0
  Tap 119;Wait-State 'F8 frame hold' 8 8
  Post-Command 51005;Wait-State 'held 2x zoom' 3840 512
  Post-Command 51005;Wait-State 'held 4x zoom' 3840 1024
  Post-Command 51005;Wait-State 'held 1x zoom' 3840 256
  Post-Command 51003;Wait-State 'swap sides' 4 4
  Key 9;Wait-State 'held Tab peek' 2 2
  [void][LiveNrProbe]::PostMessage($window,0x1C,[IntPtr]::Zero,[IntPtr]::Zero)
  Wait-State 'focus loss releases peek' 2 0;Key 9 $false
  Tap 119;Wait-State 'F8 resume' 8 0
  Tap 121;Wait-State 'F10 off clears active NR' 32 0
  Tap 9;Wait-State 'NR OFF Tab guidance' 64 64
  Tap 120;Wait-State 'F9 exits comparison' 1 0
  Key 9;Wait-State 'normal view NR OFF Tab guidance without peek' 67 64
  Key 9 $false
  Tap 120;Wait-State 'F9 enables NR and comparison' 49 49
  Tap 119;Wait-State 'hold before settings change' 8 8
  Post-Command 51523;Wait-State 'settings release hold' 8 0
  Wait-Path '75 percent comparison recovers' 'GPU native' @{NrScale=75;NrEnabled=1}
  Post-Command 51541;Wait-Path 'final defaults' 'GPU native' @{NrScale=100}
  Tap 120;Wait-State 'final live view' 1 0
  $title=Read-Title
 }
 [void][LiveNrProbe]::PostMessage($window,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
 if($AudioDeviceName -and ($title -notmatch 'audio packets [1-9][0-9]*' -or $title -match 'Audio reconnect pending')){throw "Audio capture/sync did not remain active: $title"}
 if(-not $app.WaitForExit(5000)){throw 'Normal shutdown exceeded 5 seconds'}
 if($app.ExitCode -ne 0){throw "Shutdown failed: $($app.ExitCode)"}
 [pscustomobject]@{mode='background_hardware_capture';gpu_capture=[bool]$GpuCapture;initial_seconds=$Seconds;elapsed_seconds=[Math]::Round($clock.Elapsed.TotalSeconds,2);transitions=[bool]$Transitions;creative_transitions=[bool]$CreativeTransitions;two_pass_transitions=[bool]$TwoPassTransitions;last_title=$title;exit_code=$app.ExitCode}|ConvertTo-Json
} finally {
 if($app -and -not $app.HasExited){$app.Kill();[void]$app.WaitForExit(5000)}
 if(Test-Path -LiteralPath $keyPs){Remove-Item -LiteralPath $keyPs -Recurse -Force}
 $env:DLSS_NR_TEST_SETTINGS_KEY=$priorKey
 $env:DLSS_NR_GPU_CAPTURE=$priorGpu
}
