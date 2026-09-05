param(
 [Parameter(Mandatory=$true)][string]$AppPath,
 [switch]$GpuCapture,
 [switch]$ExpectCpu,
 [switch]$Transitions,
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
 foreach($item in @{NrEnabled=1;NrTemporal=1;Width=$Width;Height=$Height;FpsNumerator=$Fps;FpsDenominator=1;FlipVertical=0}.GetEnumerator()) {
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
 [void][LiveNrProbe]::PostMessage($window,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
 if($AudioDeviceName -and ($title -notmatch 'audio packets [1-9][0-9]*' -or $title -match 'Audio reconnect pending')){throw "Audio capture/sync did not remain active: $title"}
 if(-not $app.WaitForExit(5000)){throw 'Normal shutdown exceeded 5 seconds'}
 if($app.ExitCode -ne 0){throw "Shutdown failed: $($app.ExitCode)"}
 [pscustomobject]@{mode='background_hardware_capture';gpu_capture=[bool]$GpuCapture;initial_seconds=$Seconds;elapsed_seconds=[Math]::Round($clock.Elapsed.TotalSeconds,2);transitions=[bool]$Transitions;last_title=$title;exit_code=$app.ExitCode}|ConvertTo-Json
} finally {
 if($app -and -not $app.HasExited){$app.Kill();[void]$app.WaitForExit(5000)}
 if(Test-Path -LiteralPath $keyPs){Remove-Item -LiteralPath $keyPs -Recurse -Force}
 $env:DLSS_NR_TEST_SETTINGS_KEY=$priorKey
 $env:DLSS_NR_GPU_CAPTURE=$priorGpu
}
