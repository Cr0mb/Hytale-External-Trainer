@echo off
rem Builds hytrainer-agent.jar (bootstrap) and hytrainer-core.jar (features) into ..\build\Release\agent.
setlocal
cd /d %~dp0
rem Override these if your JDK or game install lives elsewhere:
rem   set HYT_JDK=C:\path\to\jdk\bin
rem   set HYT_SERVER_JAR=C:\path\to\HytaleServer.jar
if "%HYT_JDK%"=="" set HYT_JDK=C:\Program Files\Eclipse Adoptium\jdk-25.0.4.7-hotspot\bin
if "%HYT_SERVER_JAR%"=="" set HYT_SERVER_JAR=D:\Games\Hytale_Game\install\release\package\game\latest\Server\HytaleServer.jar
set JDK=%HYT_JDK%
set SERVER=%HYT_SERVER_JAR%
if not exist "%JDK%\javac.exe" ( echo [!] javac not found at "%JDK%" - set HYT_JDK & exit /b 10 )
if not exist "%SERVER%" ( echo [!] HytaleServer.jar not found at "%SERVER%" - set HYT_SERVER_JAR & exit /b 11 )
if "%OUT%"=="" set OUT=%~dp0..\build\Release\agent
if not exist "%OUT%" mkdir "%OUT%"
if exist classes rmdir /s /q classes
mkdir classes\boot classes\core
"%JDK%\javac" --release 25 -nowarn -d classes\boot src\hytrainer\Agent.java || exit /b 1
"%JDK%\javac" --release 25 -nowarn -proc:none -cp "%SERVER%" -d classes\core src\hytrainer\core\*.java || exit /b 2
> classes\manifest.txt echo Agent-Class: hytrainer.Agent
>> classes\manifest.txt echo Main-Class: hytrainer.Agent
"%JDK%\jar" cfm "%OUT%\hytrainer-agent.jar" classes\manifest.txt -C classes\boot . || exit /b 3
"%JDK%\jar" cf "%OUT%\hytrainer-core.jar" -C classes\core . || exit /b 4
echo [+] agent jars in %OUT%
endlocal
