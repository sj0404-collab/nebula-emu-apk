@echo off
rem demo.bat — exercised by the embedded cmd.exe of the Nebula x64 emulator
setlocal
echo == batch file running under %COMSPEC%
echo cwd: %CD%   date: %DATE%
set N=0
for %%f in (*.lua *.sql *.json) do (
  set /a N+=1
  echo   file: %%f
)
echo count=%N%
if exist demo.lua (echo lua present) else (echo no lua)
if %N% GEQ 3 echo at least three files
echo == pipes and redirection
dir /b *.lua *.sql *.json | sort /r
echo first line> out.txt
echo second line>> out.txt
type out.txt | find /c "line"
echo == real programs from a batch file
jq.exe -c -M ".users | length" demo.json
lua54.exe -e "print('lua says', 6*7)"
echo == subroutine
call :sub hello world
echo errorlevel after sub: %ERRORLEVEL%
goto :eof
:sub
echo   sub got %1 %2 (from %~n0)
exit /b 7
