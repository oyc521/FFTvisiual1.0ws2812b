@echo off
REM Stop ESP32 audio streamer (kills background wifi_audio_loopback python process).
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | Where-Object { $_.CommandLine -like '*wifi_audio_loopback*' } | ForEach-Object { Write-Host ('Stopped PID ' + $_.ProcessId); Stop-Process -Id $_.ProcessId -Force }"
echo Done. (No output above = nothing was running.)
pause
