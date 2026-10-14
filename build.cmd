@echo off
setlocal
cd /d %~dp0
call agent\build_agent.cmd || ( echo [!] Agent build failed & exit /b 3 )
if not exist build mkdir build
pushd build
cmake -G "Visual Studio 18 2026" -A x64 .. || ( popd & echo [!] CMake configure failed & exit /b 1 )
cmake --build . --config Release -- /m /v:m || ( popd & echo [!] Build failed & exit /b 2 )
popd
if not exist dist mkdir dist
copy /y build\Release\HytaleTrainer.exe dist\HytaleTrainer.exe >nul || ( echo [!] Copy to dist failed & exit /b 4 )
echo [+] Built: %~dp0dist\HytaleTrainer.exe  (single file: agent and renderer are embedded)
endlocal
