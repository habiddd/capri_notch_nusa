@echo off
echo ===========================================
echo       Compilando Capri Notch (Win32/C++)
echo ===========================================

set CXX=g++
where %CXX% >nul 2>nul
if %errorlevel% neq 0 (
    if exist "C:\Strawberry\c\bin\g++.exe" (
        set "PATH=C:\Strawberry\c\bin;%PATH%"
    ) else (
        echo [ERROR] No se encontro g++ en el PATH ni en C:\Strawberry\c\bin.
        pause
        exit /b 1
    )
)

%CXX% -O2 -std=c++17 -Wall src/main.cpp -o capri_notch.exe -mwindows -static -static-libgcc -static-libstdc++ -lgdiplus -lgdi32 -lwinhttp -lwinmm -luser32 -lshell32 -lruntimeobject -lole32 -loleaut32 -lshcore -lws2_32 -lbcrypt -lcrypt32 -ldwmapi -lpsapi -lpdh -lpowrprof -llocationapi -luuid -loleacc

if %errorlevel% equ 0 (
    echo [EXITO] Compilacion completada con exito: capri_notch.exe
    echo Consumo estimado de RAM: menos de 10 MB.
) else (
    echo [ERROR] Fallo la compilacion.
    exit /b %errorlevel%
)

