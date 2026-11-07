@echo off
rem Starts the Pacitum Discord bot on Windows (needs Python 3.10+ from python.org).
cd /d "%~dp0"
if not exist .venv (
    py -3 -m venv .venv || python -m venv .venv
    .venv\Scripts\python -m pip install -r requirements.txt
)
.venv\Scripts\python pacitum_bot.py
pause
