#!/bin/sh
# Starts the Pacitum Discord bot on Linux / a server (needs Python 3.10+).
cd "$(dirname "$0")"
if [ ! -d .venv ]; then
    python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
fi
exec .venv/bin/python pacitum_bot.py
