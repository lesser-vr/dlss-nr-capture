param(
 [Parameter(Mandatory=$true)][string]$OutputDir,
 [ValidateSet(30,60)][int]$Fps=60,
 [ValidateRange(4,120)][int]$Seconds=20
)
$ErrorActionPreference='Stop'
$encoder=(Get-Command ffmpeg -ErrorAction Stop).Source
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
if(Test-Path -LiteralPath $OutputDir){throw 'Use a new output directory'}
New-Item -ItemType Directory -Path $OutputDir | Out-Null
$video=Join-Path $OutputDir 'flash-click.mkv'
# Lossless video and PCM avoid codec lookahead/priming as a source of offset.
# Small central patch flashes every two seconds; never auto-play this fixture.
$period=$Fps*2; $flashFrames=[int]($Fps/10)
$image="color=c=black:s=1280x720:r=$Fps,drawbox=x=480:y=200:w=320:h=320:color=white:t=fill:enable='lt(mod(n,$period),$flashFrames)'"
$sound="aevalsrc=if(lt(mod(t\,2)\,0.01)\,0.2*sin(2*PI*1000*t)\,0):s=48000"
& $encoder -hide_banner -loglevel error -nostdin -n -f lavfi -i $image -f lavfi -i $sound -t $Seconds -c:v ffv1 -pix_fmt yuv420p -c:a pcm_s16le $video
if($LASTEXITCODE -ne 0){throw 'Reference encoding failed; incomplete directory retained for inspection'}
$events=@(for($second=0;$second -lt $Seconds;$second+=2){[ordered]@{time_seconds=$second;video_frame=$second*$Fps;audio_sample=$second*48000}})
[ordered]@{schema_version=1;fps=$Fps;sample_rate=48000;duration_seconds=$Seconds;expected_offset_ms=0;events=$events;
 note='Source reference only, not a measurement of capture/display/speaker latency. Flashing patch and click sounds; do not auto-play. Skip first event when measuring.'} |
 ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDir 'reference.json') -Encoding UTF8
Write-Output "Created reference (not played): $video"
