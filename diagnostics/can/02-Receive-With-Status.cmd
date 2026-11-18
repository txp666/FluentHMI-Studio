@echo off
setlocal
cd /d "%~dp0"
set "FHMI_CAN_STATUS_QUERIES=1"
set "FHMI_CAN_TRACE_DIR=%~dp0logs\status-on"
echo CAN diagnostic comparison: supported status queries ON
echo Logs: %FHMI_CAN_TRACE_DIR%
start "" /wait "%~dp0FluentHMIRuntime.exe" %*
set "can_run_exit=%errorlevel%"
if not "%can_run_exit%"=="0" pause
exit /b %can_run_exit%
