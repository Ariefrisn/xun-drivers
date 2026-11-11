[Console]::OutputEncoding=[Text.Encoding]::UTF8
$logLen = (Get-Item C:\XunGpuW.log).Length
$p = Start-Process C:\xun\stage\XunGpuW\d3dtest.exe -NoNewWindow -PassThru -RedirectStandardOutput C:\xun\d3d.out -RedirectStandardError C:\xun\d3d.err
$null = $p.Handle
if (-not $p.WaitForExit(60000)) { "TIMEOUT (pid $($p.Id))" } else { "exit code $($p.ExitCode)" }
Get-Content C:\xun\d3d.out, C:\xun\d3d.err
"---- KMD log since start"
$fs = [IO.File]::Open('C:\XunGpuW.log','Open','Read','ReadWrite'); $fs.Seek($logLen,0) | Out-Null
(New-Object IO.StreamReader($fs)).ReadToEnd(); $fs.Close()
