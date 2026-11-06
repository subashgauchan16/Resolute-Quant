@echo off
title Subash Terminal
cd /d "%~dp0"

set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY where python >nul 2>nul && set "PY=python"
if not defined PY goto nopython

if exist ".venv\Scripts\python.exe" goto install
echo First run: setting up. This takes a minute or two.
%PY% -m venv .venv
if errorlevel 1 goto failed

:install
".venv\Scripts\python.exe" -m pip install --quiet --disable-pip-version-check -r requirements.txt
if errorlevel 1 goto failed

".venv\Scripts\python.exe" server.py
pause
exit /b 0

:nopython
echo Python was not found. Install it from python.org, tick "Add Python to PATH", then run this again.
pause
exit /b 1

:failed
echo Setup failed. Check your internet connection and read the message above.
pause
exit /b 1
