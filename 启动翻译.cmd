@echo off
setlocal
cd /d "%~dp0"
if not exist "dist\ClaudeTranslator\ClaudeTranslator.exe" (
  echo Native executable missing. Run build.cmd to build it first.
  pause
  exit /b 1
)
start "" "%~dp0dist\ClaudeTranslator\ClaudeTranslator.exe"