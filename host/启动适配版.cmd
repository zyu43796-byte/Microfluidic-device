@echo off
cd /d "%~dp0"
if exist "dist\DMF_Host_Adapted\DMF_Host_Adapted.exe" (
    start "" "dist\DMF_Host_Adapted\DMF_Host_Adapted.exe"
) else (
    ".venv\Scripts\python.exe" adapted_main.py
)
