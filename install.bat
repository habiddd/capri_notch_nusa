@echo off
setlocal
echo ======================================================
echo          Instalador de Capri Notch (Windows 11)
echo ======================================================

set "INSTALL_DIR=%LOCALAPPDATA%\CapriNotch"
set "MODELS_DIR=%INSTALL_DIR%\models"

echo [*] Creando carpetas de instalacion local: %INSTALL_DIR%
if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"
if not exist "%MODELS_DIR%" mkdir "%MODELS_DIR%"

echo [*] Compilando binario optimizado si es necesario...
call build.bat
if %errorlevel% neq 0 (
    echo [ERROR] Error durante la compilacion.
    pause
    exit /b %errorlevel%
)

echo [*] Instalando binario capri_notch.exe...
copy /Y "capri_notch.exe" "%INSTALL_DIR%\capri_notch.exe" >nul

echo [*] Creando acceso directo en el Escritorio...
powershell -NoProfile -Command "$ws = New-Object -ComObject WScript.Shell; $s = $ws.CreateShortcut([System.IO.Path]::Combine([Environment]::GetFolderPath('Desktop'), 'Capri Notch.lnk')); $s.TargetPath = '%INSTALL_DIR%\capri_notch.exe'; $s.WorkingDirectory = '%INSTALL_DIR%'; $s.Save()"

echo.
echo ======================================================
echo  [EXITO] Capri Notch se instalo correctamente en tu PC:
echo  Ubicacion: %INSTALL_DIR%
echo ======================================================
echo.
if "%1"=="/nopause" goto :eof
pause
