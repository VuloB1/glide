@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d %~dp0
cl /nologo /O2 /MT /EHsc /LD glide.cpp /Fe:glide.dll /link d3d11.lib dxgi.lib user32.lib || exit /b 1
cl /nologo /O2 /MT /EHsc mkicon.cpp /Fe:mkicon.exe /link gdiplus.lib ole32.lib user32.lib || exit /b 1
"%~dp0mkicon.exe" || exit /b 1
rc /nologo glide.rc || exit /b 1
cl /nologo /O2 /MT /EHsc /utf-8 ui.cpp glide.res /Fe:glide-ui.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib gdiplus.lib dwmapi.lib uxtheme.lib shell32.lib advapi32.lib || exit /b 1
cl /nologo /O2 /MT /EHsc test.cpp /Fe:test.exe /link user32.lib || exit /b 1
rc /nologo setup.rc || exit /b 1
cl /nologo /O2 /MT /EHsc /utf-8 setup.cpp setup.res /Fe:glide-setup.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib gdiplus.lib dwmapi.lib ole32.lib shell32.lib advapi32.lib uuid.lib || exit /b 1
del *.obj mkicon.exe 2>nul
