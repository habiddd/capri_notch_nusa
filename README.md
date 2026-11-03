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

