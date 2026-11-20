@echo off
setlocal
set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "VERSION=%~1"
if "%VERSION%"=="" (
  echo [edvr] usage: package.bat ^<version^> [--no-dlss]
  exit /b 1
)
rem Packaging does not build. It packages the outputs of the last full build,
rem which must match the tree now: run build.bat --wait-lock first.
echo [edvr] checking the last full build against this tree
python "%ROOT%\tools\build_receipt.py" --verify-tree "%ROOT%\build\full_build_receipt.json" --root "%ROOT%" --require-clean || (
  echo [edvr] ERROR: the last full build does not match this tree. Run build.bat --wait-lock, then package.bat again.
  exit /b 1
)
echo [edvr] staging native release
call python "%ROOT%\tools\package_native.py" "%VERSION%" %2 || exit /b 1
echo [edvr] dist\ed-up-manifest-v2.json and dist\ed-up-manifest.json (the schema-1 bridge) are unsigned, and a release without both .sig files is not offered as an update.
echo [edvr] sign both in your own terminal: python tools\sign_manifest.py sign dist\ed-up-manifest-v2.json
echo [edvr]                                 python tools\sign_manifest.py sign dist\ed-up-manifest.json
exit /b 0
