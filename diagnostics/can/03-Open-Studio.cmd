@echo off
setlocal
cd /d "%~dp0"
set "FHMI_CAN_STATUS_QUERIES=0"
set "FHMI_CAN_TRACE_DIR=%~dp0logs\studio-status-off"
echo CAN diagnostic Studio: status queries OFF
echo Logs: %FHMI_CAN_TRACE_DIR%
start "" /wait "%~dp0FluentHMIStudio.exe" %*
set "can_run_exit=%errorlevel%"
if not "%can_run_exit%"=="0" pause
exit /b %can_run_exit%
