@echo off
setlocal
cd /d "%~dp0webapp"
if errorlevel 1 (
  echo [ERROR] Could not open the webapp directory. Keep this launcher in the repository root.
  pause
  exit /b 1
)
if not exist ".env" (
  echo [ERROR] webapp\.env is missing. Copy webapp\.env.example and fill in the broker settings.
  pause
  exit /b 1
)
if not exist "node_modules\" (
  echo [SETUP] Installing npm dependencies...
  call npm ci --include=dev
  if errorlevel 1 (pause & exit /b 1)
)
echo [BUILD] Building current React frontend...
call npm run build
if errorlevel 1 (pause & exit /b 1)
echo [START] Open http://localhost:3000/ in your browser. Keep this terminal open.
start "" "http://localhost:3000/"
call npm start
pause
