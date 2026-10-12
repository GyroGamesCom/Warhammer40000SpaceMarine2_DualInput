@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /MT /O2 /GS- /W3 /D_CRT_SECURE_NO_WARNINGS /LD dinput8.c /Fe:dinput8.dll /link /DEF:dinput8.def user32.lib kernel32.lib || exit /b 1
del dinput8.obj 2>nul
dumpbin /exports dinput8.dll
