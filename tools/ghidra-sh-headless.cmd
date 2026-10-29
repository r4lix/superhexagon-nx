@echo off
:: Headless Ghidra MCP server for SuperHexagon.exe on port 8091 (8089 = speedracer session, 8090 = i76).
setlocal
set "SW=%~dp0..\..\..\.."
set "GHIDRA_HOME=%SW%\tools\ghidra\ghidra_12.1.4_PUBLIC"
set "GHIDRA_MCP_FILE_ROOT=%SW%"
call "%GHIDRA_HOME%\support\launch.bat" fg jdk GhidraMCP-SH 4G "-XX:ParallelGCThreads=2" com.xebyte.headless.GhidraMCPHeadlessServer --port 8091 %*
