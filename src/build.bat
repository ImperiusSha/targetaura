@echo off
REM ---------------------------------------------------------------------------
REM Build targetaura.dll  (Ashita v4 plugin, 32-bit / x86).
REM Adjust the two paths below for your machine, then run this from a normal cmd:
REM   VCVARS     = your Visual Studio 2022 vcvars32.bat (x86 developer environment)
REM   ASHITA_SDK = your Ashita v4 plugin SDK folder (the one containing Ashita.h)
REM The compiled targetaura.dll appears next to this script; copy it to <Ashita>\plugins\.
REM ---------------------------------------------------------------------------
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
set "ASHITA_SDK=C:\HorizonXI\Game\plugins\sdk"

call "%VCVARS%"
cd /d "%~dp0"
cl /nologo /LD /MT /std:c++20 /EHa /O2 /DWIN32 /DNDEBUG /I "%ASHITA_SDK%" targetaura.cpp /link /DEF:targetaura.def /OUT:targetaura.dll
echo BUILD_EXITCODE=%ERRORLEVEL%
