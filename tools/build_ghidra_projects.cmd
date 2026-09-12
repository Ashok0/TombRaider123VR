@echo off
REM Pre-import the four binaries into the Ghidra projects that re-mcp expects.
REM
REM re-mcp-ghidra's open() creates its project ALONGSIDE the binary:
REM
REM     project_dir  = <binary dir>\ghidra_projects
REM     project_name = <binary file name, extension included>
REM     .gpr         = <binary dir>\ghidra_projects\<name>.gpr
REM
REM (see re_mcp_ghidra\session.py). If the .gpr already exists open() reuses it
REM instead of importing, so building them here means the first MCP call is
REM instant rather than a multi-minute auto-analysis -- and, more importantly,
REM the analysis runs with each binary's PDB sitting right next to it, so
REM Ghidra's PDB Universal analyzer applies the real symbols and types instead
REM of inventing FUN_/DAT_ names.
REM
REM Safe to re-run: analyzeHeadless will not re-import a program already in the
REM project. Delete PDB\ghidra_projects to start over.
REM
REM TWO THINGS THAT BIT ON THE FIRST ATTEMPT, both fixed below:
REM
REM   1. The project path must be FULLY RESOLVED. Passing "%~dp0..\PDB\..."
REM      leaves a ".." in the path and Ghidra rejects it outright:
REM        "Path element starting with '.' is not permitted"
REM      (ghidra.util.NamingUtilities.checkName). pushd/%CD%/popd resolves it.
REM
REM   2. analyzeHeadless.bat ends in a PAUSE. Run non-interactively it blocks
REM      forever on "Press any key to continue" instead of exiting, which looks
REM      exactly like a very slow analysis. Feeding it NUL on stdin is what
REM      stops that.
setlocal

set "GHIDRA=C:\Ghidra\support\analyzeHeadless.bat"
if not exist "%GHIDRA%" (
  echo Ghidra not found at %GHIDRA%
  exit /b 1
)

REM Resolve <repo>\PDB to an absolute path with no "." or ".." elements.
pushd "%~dp0.." || exit /b 1
set "ROOT=%CD%"
popd
set "SRC=%ROOT%\PDB"
set "PROJ=%SRC%\ghidra_projects"

if not exist "%SRC%" (
  echo No PDB folder at %SRC%
  exit /b 1
)
if not exist "%PROJ%" mkdir "%PROJ%"

echo Importing into %PROJ%

for %%B in (tomb123.exe tomb1.dll tomb2.dll tomb3.dll) do (
  echo.
  echo ==================== %%B ====================
  call "%GHIDRA%" "%PROJ%" "%%B" -import "%SRC%\%%B" -analysisTimeoutPerFile 1800 < NUL
)

echo.
echo Done. Projects in %PROJ%
