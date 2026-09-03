param([Parameter(Mandatory=$true)][string]$AppPath)
$ErrorActionPreference = 'Stop'
$keyRelative = "Software\DlssNrCapture\Tests\CTest-$PID"
$keyPs = "HKCU:\$keyRelative"
$env:DLSS_NR_TEST_SETTINGS_KEY = $keyRelative
$script:app = $null
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class RegressionUi {
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
 [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr h);
 [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr h);
 [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr h, int p);
 [DllImport("user32.dll")] public static extern uint GetMenuState(IntPtr h, uint id, uint f);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetMenuString(IntPtr h, uint i, StringBuilder s, int n, uint f);
 [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll", EntryPoint="GetWindowLongW")] public static extern int GetWindowLong(IntPtr h, int n);
 [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
}
"@
function Start-TestApp {
  $p = Start-Process -FilePath $AppPath -WorkingDirectory (Split-Path $AppPath) -PassThru
  for ($i=0; $i -lt 100; $i++) { Start-Sleep -Milliseconds 100; $p.Refresh(); if ($p.HasExited) { throw "App exited early: $($p.ExitCode)" }; if ($p.MainWindowHandle -ne 0) { return $p } }
  throw 'App window did not appear within 10 seconds'
}
function Stop-TestApp($p) {
  if ($p -and -not $p.HasExited) { $p.Kill(); $p.WaitForExit(5000) | Out-Null }
  Start-Sleep -Milliseconds 300
  Get-CimInstance Win32_Process -Filter "Name='dlss-nr-worker.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.ParentProcessId -eq $p.Id } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}
try {
  Remove-Item -LiteralPath $keyPs -Recurse -Force -ErrorAction SilentlyContinue
  $script:app = Start-TestApp
  $menu = [RegressionUi]::GetMenu([IntPtr]$script:app.MainWindowHandle)
  if ($menu -eq [IntPtr]::Zero) { throw 'Top-level menu missing' }
  if ([RegressionUi]::GetDpiForWindow([IntPtr]$script:app.MainWindowHandle) -lt 96) { throw 'Window is not using a valid monitor DPI' }
  $labels = @()
  for ($i=0; $i -lt [RegressionUi]::GetMenuItemCount($menu); $i++) { $b=[Text.StringBuilder]::new(128); [void][RegressionUi]::GetMenuString($menu,[uint32]$i,$b,128,0x400); $labels += $b.ToString() }
  foreach ($expected in @('Capture device','Video format','Resolution','Frame rate','Audio capture','Processing','Neural Rendering','View')) {
    if ($labels -notcontains $expected) { throw "Menu missing: $expected" }
  }
  foreach ($command in @(49001,49002,51001,51102,51204,51301,51401)) { [void][RegressionUi]::SendMessage([IntPtr]$script:app.MainWindowHandle,0x111,[IntPtr]$command,[IntPtr]::Zero) }
  Start-Sleep -Milliseconds 500

  $saved = Get-ItemProperty -LiteralPath $keyPs
  if ($saved.AlwaysOnTop -ne 1 -or $saved.AutoSizeToResolution -ne 1 -or $saved.NrTemporal -ne 0 -or $saved.NrStyle -ne 2 -or $saved.NrPreset -ne 4 -or $saved.NrIntensity -ne 75 -or $saved.NrWaitMs -ne 16) { throw 'Menu settings were not persisted correctly' }
  if (([RegressionUi]::GetWindowLong([IntPtr]$script:app.MainWindowHandle,-20) -band 8) -eq 0) { throw 'Always-on-top style was not applied' }
  Stop-TestApp $script:app; $script:app = Start-TestApp; Start-Sleep -Milliseconds 1500
  $script:app.Refresh()
  if ($script:app.MainWindowTitle -match '(\d+)x(\d+) @') {
    [void][RegressionUi]::SetThreadDpiAwarenessContext([IntPtr](-4))
    $rect = New-Object RegressionUi+RECT
    [void][RegressionUi]::GetClientRect([IntPtr]$script:app.MainWindowHandle, [ref]$rect)
    if (($rect.R-$rect.L) -ne [int]$Matches[1] -or ($rect.B-$rect.T) -ne [int]$Matches[2]) {
      throw "DPI-aware client size mismatch: $($rect.R-$rect.L)x$($rect.B-$rect.T), expected $($Matches[1])x$($Matches[2])"
    }
  }
  if (([RegressionUi]::GetWindowLong([IntPtr]$script:app.MainWindowHandle,-20) -band 8) -eq 0) { throw 'Always-on-top setting was not restored' }
  if (-not $script:app.Responding) { throw 'App is not responding after restart' }
  Write-Host "app smoke checks passed ($($labels.Count) top-level menus)"
} finally {
  Stop-TestApp $script:app
  Remove-Item -LiteralPath $keyPs -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item Env:DLSS_NR_TEST_SETTINGS_KEY -ErrorAction SilentlyContinue
}
