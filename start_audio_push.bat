@echo off
REM ESP32 audio streamer: auto-discover device, open web console, push PC audio via UDP.
REM Close this window (or Ctrl+C) to stop.
python "%~dp0tools\wifi_audio_loopback.py"
if errorlevel 1 (
  echo.
  echo Failed to start. Make sure Python is installed and on PATH.
  echo First time deps install:  python -m pip install pyaudiowpatch numpy
)
pause
