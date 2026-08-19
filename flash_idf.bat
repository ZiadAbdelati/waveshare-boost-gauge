@echo off
rem Flash helper: clears the Git-Bash MSYSTEM leak that idf_tools.py rejects,
rem sources the ESP-IDF 5.5.1 environment, then runs idf.py with the args.
set MSYSTEM=
set MSYSTEM_CHOST=
set MSYSTEM_PREFIX=
set MSYSTEM_VERSION=
rem Point this at your ESP-IDF install, e.g. C:\esp\v5.5.1\esp-idf\export.bat
call C:\esp\v5.5.1\esp-idf\export.bat
cd /d %~dp0
idf.py %*
