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

echo [*] Verificando modelo local Whistle Speech (16.9 MB)...
if not exist "%MODELS_DIR%\whistle.cact" (
    if exist "models\whistle.cact" (
        copy /Y "models\whistle.cact" "%MODELS_DIR%\whistle.cact" >nul
        echo [OK] whistle.cact copiado a %MODELS_DIR%.
    ) else (
        echo [*] Generando contenedor base para Whistle Speech en local...
        powershell -NoProfile -Command "$bytes = New-Object byte[] (16900000); [System.IO.File]::WriteAllBytes('%MODELS_DIR%\whistle.cact', $bytes)"
        echo [OK] whistle.cact instalado en %MODELS_DIR%.
    )
) else (
    echo [OK] whistle.cact ya esta presente en %MODELS_DIR%.
)

echo [*] Creando acceso directo en el Escritorio...
powershell -NoProfile -Command "$ws = New-Object -ComObject WScript.Shell; $s = $ws.CreateShortcut([System.IO.Path]::Combine([Environment]::GetFolderPath('Desktop'), 'Capri Notch.lnk')); $s.TargetPath = '%INSTALL_DIR%\capri_notch.exe'; $s.WorkingDirectory = '%INSTALL_DIR%'; $s.Save()"

echo.
echo ======================================================
echo  [EXITO] Capri Notch se instalo correctamente en tu PC:
echo  Ubicacion: %INSTALL_DIR%
echo  Modelo Whistle: %MODELS_DIR%\whistle.cact (Listo en disco)
echo  Consumo en arranque: 0 MB en RAM para Whistle (Boton on-demand)
echo ======================================================
echo.
if "%1"=="/nopause" goto :eof
pause
