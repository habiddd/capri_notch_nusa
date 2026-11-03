# Capri Notch 🏝️

Aplicación ultraligera de Dynamic Notch / Dynamic Island para Windows 11 escrita en C++ nativo (Win32 + GDI+), diseñada con un consumo estricto de memoria RAM inferior a 25 MB.

---

## ⚡ Características Principales

   - Píldora colapsada flotante (`220x34 px`) en la parte superior central de la pantalla. Muestra, por prioridad:
     aviso de cargador (3 s), pomodoro en curso (cuenta atrás y barra de progreso), música (carátula, título o letra y ecualizador)
     y, en reposo, los ojos de la mascota con el próximo evento de hoy, las tareas pendientes o la fecha, más el reloj.
   - Píldora colapsada flotante (`220x34 px`) en la parte superior central de la pantalla.
   - Expansión animada fluida a 60 FPS (`460x250 px`) al hacer clic o mediante comando por voz.
   - Ventana por capas (`WS_EX_LAYERED`) con canal alfa por píxel y estética oscura fluida.

2. **🎵 Música y Letras (LRCLIB Open-Source)**:
   - Detección del reproductor activo (Spotify, navegadores web, etc.).
   - Controles multimedia globales (`Anterior`, `Reproducir/Pausar`, `Siguiente`).
   - Búsqueda integrada de letras en la API abierta y gratuita [LRCLIB](https://lrclib.net).

3. **⏱️ Pomodoro y Temporizador**:
   - Ciclos de 25 minutos de enfoque y 5 minutos de descanso.
   - Barra de progreso visual y contador de sesiones completadas.
   - Alerta sonora nativa del sistema al finalizar.

4. **📅 Calendario y Tareas (To-Do)**:
   - Calendario grande (`760x400 px`) con mini-mes, vistas **Agenda** (próximos 14 días), **Día**, **Semana** (rejilla horaria con bloques de duración) y **Mes**. Flechas y rueda del ratón avanzan o desplazan; clic en un hueco de la rejilla crea un evento ahí.
   - **Nuevo evento** (botón de la barra lateral, hueco de la rejilla o lápiz de la tira inferior): ventana emergente con título, fecha (‹ ›, Hoy, Mañana), hora de inicio y fin (pasos de 15 min) o *Todo el día*, repetición, categoría, aviso (ninguno, al empezar, 10 min, 30 min, 1 h) y notas. Atajos: `Tab` cambia entre título y notas, `Enter` guarda, `Esc` cancela. Si cierras el panel con la ventana abierta, el borrador se conserva.
   - El título entiende lenguaje natural al guardar: `Dentista mañana 15:30-16:30 #salud cada semana`.
     Hora `15:30`, `15:30-16:30` o `15h`; fecha `hoy`, `mañana`, `pasado mañana`, un día de la semana o `15/11`; categoría `#trab`, `#salud`...; repetición `diario`, `semanal`, `mensual`, `anual` (o `cada día/semana/mes/año`).
   - 6 categorías con color (General, Trabajo, Personal, Salud, Estudio, Social) que se pueden ocultar con un clic. Un evento seleccionado se edita (ventana), mueve de día, recolorea, repite o borra desde la tira inferior (los repetidos cambian en toda la serie).
   - Recordatorio en la píldora según el aviso del evento (antes y a la hora; 8 s y un pitido).
   - Los eventos se guardan en `%LOCALAPPDATA%\capri_events.txt` (los formatos antiguos se siguen leyendo; las notas van tras un tabulador).
   - Lista interactiva de tareas con casillas de verificación persistidas localmente en disco.

5. **🤖 Compañero IA (Cloud / Local)**:
   - Cliente HTTP WinHTTP nativo compatible con APIs OpenAI / DeepSeek / Ollama.
   - Consumo de RAM local adicional: ~0 MB (la inferencia se delega a la nube o servidor local).

6. **🎙️ Wake-Up Call (Cactus AI - Whistle Speech)**:
   - Se instala en disco local (`%LOCALAPPDATA%\CapriNotch\models\whistle.cact`, 16.9 MB).
   - **Carga bajo demanda (Lazy-Loading)**: Al arrancar la app consume **0 MB en RAM**. Solo se carga a memoria cuando el usuario presiona el botón en la pestaña de IA.
   - Al desactivarlo con el mismo botón, libera inmediatamente los 16.9 MB de la memoria y devuelve las páginas al sistema operativo.

---

## 🛠️ Instalación y Compilación

### Instalación Automática en Windows 11:
Doble clic en `install.bat` o desde CMD:
```cmd
install.bat
```
Esto compila el binario, instala `capri_notch.exe` y el modelo local `whistle.cact` en `%LOCALAPPDATA%\CapriNotch\`, y crea un acceso directo en tu Escritorio.

### Solo Compilar (Desarrollo):
```cmd
build.bat
```

---

## Sincronizar calendario

Capri Notch puede subir y bajar tus eventos de **Google Calendar** y **Outlook** (calendario principal). Google se conecta con un script tuyo en Apps Script (no hace falta Google Cloud); Outlook, con tu propio registro de Microsoft. No hay ninguna clave compartida dentro del programa.

### 1. Google (script de Apps Script)
1. En Capri Notch: Configuración → **Cuentas** → **Copiar script**.
2. Entra en [script.google.com](https://script.google.com) → **Nuevo proyecto**, borra lo que haya, pega (Ctrl+V) y guarda.
3. Panel izquierdo: **Servicios +** → **Google Calendar API** → Añadir.
4. **Implementar → Nueva implementación** → tipo **Aplicación web** → **Ejecutar como: Yo** → **Quién tiene acceso: Cualquier usuario** → Implementar → Autorizar acceso. Si sale "Google no ha verificado esta aplicación": **Configuración avanzada → Ir a … (no seguro)**; es tu propio script.
5. Copia la **URL de la aplicación web** (termina en `/exec`), pégala en Capri Notch y pulsa **Conectar**.

La URL y la clave dan acceso a tu calendario: no las compartas. Copiar el script otra vez reutiliza la misma clave. Para revocarlo: **Desconectar** en Capri Notch y en Apps Script, **Gestionar implementaciones → Archivar**. Tras desconectar, la siguiente vez hay que copiar el script de nuevo y volver a implementarlo.

### 2. Outlook (Microsoft)
1. Entra en el portal de Azure ([entra.microsoft.com](https://entra.microsoft.com)) → **Registros de aplicaciones → Nuevo registro**.
2. Tipo de cuenta: **cuentas de cualquier directorio y cuentas personales de Microsoft**.
3. En **Autenticación**: **Agregar plataforma → Aplicaciones móviles y de escritorio** con el URI `http://localhost`.
4. En **Autenticación → Configuración avanzada**: **Permitir flujos de cliente público = Sí**.
5. En **Permisos de API**: delegados `Calendars.ReadWrite` y `User.Read`.
6. Copia el **Id. de aplicación (cliente)** y pégalo en Configuración → **Cuentas** → **ID de aplicación de Microsoft**.

### 3. Capri Notch
1. Abre la configuración desde el icono de la bandeja (o el engranaje del notch) y ve a **Cuentas**.
2. Outlook: pulsa **Conectar** en su fila. Se abre el navegador para iniciar sesión y dar permiso. Al volver, el punto pasa a verde. (Google se conecta desde el paso 1.)
3. Desde ahí mismo: **Sincronizar** o **Desconectar**.

El punto de cada servicio: gris sin conectar, verde conectado, ámbar sincronizando, rojo error. La URL y la clave de Google, y el token de Outlook, se guardan cifrados con DPAPI en `%LOCALAPPDATA%\capri_sync_google.bin` y `capri_sync_outlook.bin`; no se escriben en registros.

### 4. Calendario por enlace (ICS, solo lectura)
Para Outlook sin registrar nada (o cualquier calendario con enlace ICS):
1. Outlook.com: Configuración → Calendario → **Calendarios compartidos** → **Publicar un calendario** → elige el calendario → copia el **enlace ICS**. En Google: Configuración del calendario → **Dirección secreta en formato iCal**.
2. En Capri Notch: Configuración → **Cuentas** → pega el enlace en **Enlace ICS** (Ctrl+V) → **Conectar**. Acepta `https://` y `webcal://`.

Los eventos aparecen con una marca **G** (si el enlace es de Google) o **O** y solo se puede cambiar su color: no se editan, mueven, repiten ni borran desde Capri. Outlook puede tardar horas en publicar los cambios en el enlace. El enlace es secreto: se guarda cifrado en `%LOCALAPPDATA%\capri_sync_ics.bin` y **Desconectar** lo borra.

### 5. Samsung Calendar
Samsung no tiene API pública. En el móvil, abre **Samsung Calendar → Ajustes → Calendarios/Cuentas** y guarda los eventos en la cuenta de **Google** u **Outlook**. Así llegan a Capri Notch.

### Qué se sincroniza
- Título, notas, fecha, hora (también de todo el día), categoría (en Google), aviso y repetición de los eventos que creas en Capri.
- Ventana: desde 30 días atrás hasta 180 días después. Fuera de ella no se descarga nada.
- Cada 5 minutos, y 2 segundos después de editar un evento sincronizado.
- Si cambias un evento y aún no se ha subido, gana tu cambio hasta que se suba.

### Qué no se sincroniza
- Calendarios que no son el principal, invitados, lugar y archivos adjuntos.
- Eventos de varios días: solo se ven en su primer día.
- Cambiar la repetición de una serie que ya está en el servicio (se muestra, pero no se edita desde Capri).
- Las series creadas en Google aparecen como eventos sueltos dentro de la ventana.
- Enlace ICS: las repeticiones se expanden hasta 1000 apariciones por serie; las reglas poco comunes (p. ej. `BYSETPOS` o días con número como "el 2.º jueves") solo muestran la primera aparición.
- Desconectar borra de Capri los eventos de ese servicio (antes pregunta si hay cambios sin subir).

---

## Configuración

Se abre de tres formas: con el icono de la bandeja (clic), con clic derecho sobre el icono o sobre la píldora (**Configuración**), o con el engranaje de la fila de pestañas del panel expandido, que abre directamente la sección de esa pestaña. Los cambios se aplican al instante y se guardan en `%LOCALAPPDATA%\capri_settings.ini`. Sin ese archivo se usan los valores por defecto de siempre; borrarlo restablece todo.

El menú del icono de la bandeja tiene **Configuración**, **Ocultar notch** / **Mostrar notch**, **Mover al otro monitor** y **Salir de Capri Notch**.

- **General**: iniciar con Windows (usuario actual), abrir al pasar el ratón, retraso al abrir y al cerrar, esconder al hacer clic al lado, ocultar en pantalla completa (desactivado por defecto), mover al otro monitor.
- **Píldora**: ojos, dormir tras N minutos sin uso, ecualizador, aviso del cargador y letra en la píldora.
- **Música**: la misma opción de letra y un recordatorio de que las letras son de LRCLIB.
- **Pomodoro**: duración de enfoque (5 a 90 min) y descanso (1 a 30 min), y sonido al terminar. Con el temporizador parado, el cambio se aplica ya; en marcha, a partir del siguiente ciclo.
- **Calendario**: vista inicial, aviso y categoría por defecto para los eventos nuevos, y sonido de recordatorios.
- **Cuentas**: estado de Google, Outlook y el enlace ICS, con Conectar, Desconectar y Sincronizar ahora. Google: copiar el script y pegar la URL de la aplicación web (ver "Sincronizar calendario"). El ID de Microsoft se guarda en `capri_sync.ini`. Intervalo de sincronización de 5, 15 o 30 min.
- **IA**: proveedor (OpenAI, DeepSeek, Ollama local o Personalizado), modelo y clave de API. Con Personalizado se editan también el servidor, la ruta, el puerto y HTTPS. Ollama no necesita clave.
- **Acerca de**: versión, memoria en uso, abrir la carpeta de datos y salir.

La clave de API se guarda cifrada con DPAPI en `%LOCALAPPDATA%\capri_ai_key.bin`, ligada a tu cuenta de Windows. Nunca va al archivo de ajustes ni a los registros.

