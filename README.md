# Capri Notch 🏝️

Aplicación ultraligera de Dynamic Notch / Dynamic Island para Windows 11 escrita en C++ nativo (Win32 + GDI+), diseñada con un consumo estricto de memoria RAM inferior a 25 MB.

---

## ⚡ Características Principales

   - Píldora colapsada flotante (`220x34 px`) en la parte superior central de la pantalla. Muestra, por prioridad:
     aviso de cargador (3 s), pomodoro en curso (cuenta atrás y barra de progreso), música (carátula, título o letra y ecualizador)
     y, en reposo, los ojos de la mascota con el próximo evento de hoy, las tareas pendientes o la fecha, más el reloj.
     La pestaña que dejes abierta también cuenta: con **Pomodoro** se ve el temporizador aunque esté parado, y con **Sistema**,
     CPU, GPU, RAM y los vatios de la batería (la píldora se ensancha para que quepan; nunca crece en alto).
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
   - **Nuevo evento** (botón de la barra lateral, hueco de la rejilla o lápiz de la tira inferior): ventana emergente con título, fecha (‹ ›, Hoy, Mañana), hora de inicio y fin (pasos de 15 min) o *Todo el día*, repetición, categoría, avisos (varios a la vez, ver abajo) y notas. Atajos: `Tab` cambia entre título y notas, `Enter` guarda, `Esc` cancela. Si cierras el panel con la ventana abierta, el borrador se conserva.
   - El título entiende lenguaje natural al guardar: `Dentista mañana 15:30-16:30 #salud cada semana`.
     Hora `15:30`, `15:30-16:30` o `15h`; fecha `hoy`, `mañana`, `pasado mañana`, un día de la semana o `15/11`; categoría `#trab`, `#salud`...; repetición `diario`, `semanal`, `mensual`, `anual` (o `cada día/semana/mes/año`).
   - 6 categorías con color (General, Trabajo, Personal, Salud, Estudio, Social) que se pueden ocultar con un clic. Un evento seleccionado se edita (ventana), mueve de día, recolorea, repite o borra desde la tira inferior (los repetidos cambian en toda la serie).
   - Varios avisos por evento (ninguno, al empezar, 10 min, 30 min, 1 h; se pueden elegir a la vez). Recordatorio en la píldora según cada aviso (8 s y un pitido).
   - Las tareas viven en la vista **Agenda** (ya no tienen pestaña propia): tras los eventos del día seleccionado va la lista **Tareas**, con casilla, subtareas (botón +), contador de pendientes y X para borrar, y debajo el campo para añadir. La rueda desplaza la lista entera.
   - Tareas con fecha: `Informe mañana 10:00` escrito en ese campo crea un evento marcado como tarea (con hora o sin ella: sin hora, la fecha basta). En la Agenda sale una sola vez, en la lista Tareas (un clic en su texto la selecciona para moverla o editarla desde la tira inferior); en Día, Semana y Mes aparece con ○ (✓ cuando está hecha). El botón **Tarea** de la ventana del evento convierte en tarea los eventos nuevos y los de Capri; los eventos que ya están en Google u Outlook no pueden convertirse. Las tareas son solo de Capri y nunca se suben a Google u Outlook. Se pueden ocultar en el calendario desde Configuración → Calendario.
   - Los eventos se guardan en `%LOCALAPPDATA%\capri_events.txt` (los formatos antiguos se siguen leyendo; las notas van tras un tabulador).
   - Lista interactiva de tareas con casillas de verificación persistidas localmente en disco.

5. **🤖 Compañero IA (Cloud / Local)**:
   - Cliente HTTP WinHTTP nativo compatible con APIs OpenAI / DeepSeek / Ollama.
   - Consumo de RAM local adicional: ~0 MB (la inferencia se delega a la nube o servidor local).

6. **🎙️ Dictado local (Cactus AI - Whistle)**:
   - [Whistle](https://huggingface.co/Cactus-Compute/whistle) es un modelo de voz a texto de 16,9 MB que corre en la CPU, sin internet ni claves, en español, inglés, alemán, francés, italiano, neerlandés y polaco.
   - Se descarga desde **Configuración → IA → Whistle (voz a texto)** (unos 18 MB de Hugging Face: el modelo y el motor oficial de Cactus `libcactus.dll` 4.1.1, que se comprueba con SHA-256 antes de usarse). Quedan en `%LOCALAPPDATA%\CapriNotch\`.
   - Abrir la pestaña **IA** no enciende nada: Whistle (y Mirage y la Cámara) se encienden a mano desde su barra. Con Whistle encendido transcribe lo que dices; el texto aparece en la tarjeta (lo confirmado y, con puntos suspensivos, lo que aún puede cambiar). La barra de Whistle lo apaga y lo enciende. Se apaga solo al cambiar de pestaña o cerrar el notch, y mientras escucha el notch no se cierra.
   - [Mirage](https://huggingface.co/Cactus-Compute/mirage) es un modelo de visión de 21,6 MB sobre el mismo motor de Cactus. Se descarga desde **Configuración → IA → Mirage (ve tu pantalla)**. En la pestaña **IA**, los chips **Whistle | Mirage** eligen el modelo; con Mirage la barra comparte la pantalla: un proceso aparte (`capri_notch.exe --mirage`) mira el monitor del notch cada dos segundos y la tarjeta muestra la escena y el texto que lee. Nada sale del PC. Nunca arranca solo, y termina al cambiar de pestaña o cerrar el notch.
   - Mirage describe y lee la pantalla, no responde preguntas.
   - El modelo nunca se carga en el notch: corre en un proceso aparte (`capri_notch.exe --whistle`). Apagarlo termina ese proceso y Windows recupera toda su memoria al instante.
   - El chip **Cámara** de la pestaña **IA** controla el PC con la mano. Elige el motor en **Motor de manos**: MediaPipe (rápido, 8 MB de Hugging Face, CPU) o RTMPose (más preciso, 59 MB de OpenMMLab, usa la GPU con DirectML si la hay). Los dos corren en el ONNX Runtime de Windows 11 y cada archivo se comprueba con SHA-256. Los pesos de RTMPose se entrenaron con datos públicos de manos, incluido COCO: uso personal. Gestos (estáticos, grabados con tu mano en `src/gesture_templates.h`; cuentan tras 0,3 s): índice y medio juntos hacia arriba sube el volumen y hacia abajo lo baja (repite mientras los mantienes); pulgar hacia tu derecha pasa a la siguiente canción; índice y medio hacia tu izquierda vuelve a la anterior; palma abierta 1 s bloquea el PC con 3 s para cancelar. Los ejemplos se grabaron con MediaPipe. La cámara se queda en un proceso aparte (`capri_notch.exe --camera`) y sigue activa con el notch cerrado hasta que pulses su barra. Nada sale del PC. Windows bloquea los clics hacia ventanas ejecutadas como administrador.

7. **Clima y sistema (pestañas nuevas)**:
   - **Clima**: temperatura, sensación térmica, humedad, viento y máxima/mínima de hoy para tu ubicación: la de Windows si la tienes activada; si no, una aproximada por tu IP ([ipwho.is](https://ipwho.is)), o la ciudad que escribas en Configuración → Clima. Usa la API abierta [Open-Meteo](https://open-meteo.com), sin clave: la ciudad que escribes se envía a su buscador de ubicaciones, y a Open-Meteo van las coordenadas. Con la ubicación automática y la de Windows desactivada, [ipwho.is](https://ipwho.is) ve tu IP. Todo ello solo mientras la pestaña Clima está abierta, como mucho cada 15 minutos.
   - **Sistema**: uso de CPU (con su gráfica del último minuto y su temperatura), temperatura de la GPU, memoria, espacio libre en C:, batería y los vatios que entran o salen de ella. Solo se mide mientras la pestaña Sistema está abierta.

8. **Archivos (estante y envío al móvil)**:
   - Arrastra archivos desde el Explorador o el escritorio sobre el notch: se abre solo en la pestaña **Archivos** antes de soltar. Si te vas sin soltar, se cierra.
   - El botón **Modo** cambia entre dos vistas. *Guardar*: lo que sueltas se **copia** a `%LOCALAPPDATA%\CapriNotch\shelf\` y el original no se toca (las carpetas no se guardan); un clic abre la copia y la X la manda a la Papelera. *Enviar*: no muestra el estante, solo una zona donde soltar; lo que sueltas se manda a tu móvil con [Quick Share](https://www.android.com/better-together/quick-share-app/) de Google (debe estar instalado) y no se guarda. La transferencia la hace Quick Share en su propio proceso.
   - En *guardar* los archivos se ven como una cuadrícula de miniaturas sin nombre (hasta 4, grandes en una fila; con más, pequeñas en dos filas) o como una lista de nombres (Configuración → General → **Archivos: miniaturas**). Las miniaturas se cargan al pintar y se liberan al cerrar el notch o salir de la pestaña. La rueda del ratón desplaza la lista.

---

## 🛠️ Instalación y Compilación

### Instalación Automática en Windows 11:
Doble clic en `install.bat` o desde CMD:
```cmd
install.bat
```
Esto compila el binario, instala `capri_notch.exe` en `%LOCALAPPDATA%\CapriNotch\`, y crea un acceso directo en tu Escritorio.

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
- Varios avisos: solo el más largo se sube a Google y Outlook; los demás avisan solo en Capri.

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

Esc cierra la ventana. Narrador lee el nombre y el estado de cada control.

El menú del icono de la bandeja tiene **Configuración**, **Ocultar notch** / **Mostrar notch**, **Mover al otro monitor** y **Salir de Capri Notch**.

- **General**: iniciar con Windows (usuario actual), abrir al pasar el ratón, retraso al abrir y al cerrar, esconder al hacer clic al lado, ocultar en pantalla completa (desactivado por defecto), miniaturas de la pestaña Archivos, mover al otro monitor.
- **Píldora**: dormir tras N minutos sin uso, ojos, aviso del cargador, y lo que muestra en su vista compacta: letra, carátula, artista y ecualizador con música (letras de LRCLIB, gratis y sin cuenta), Pomodoro y Sistema.
- **Pomodoro**: duración de enfoque (5 a 90 min) y descanso (1 a 30 min), y sonido al terminar. Con el temporizador parado, el cambio se aplica ya; en marcha, a partir del siguiente ciclo.
- **Calendario**: vista inicial, avisos y categoría por defecto para los eventos nuevos, sonido de recordatorios y tareas con fecha en el calendario.
- **Cuentas**: estado de Google, Outlook y el enlace ICS, con Conectar, Desconectar y Sincronizar ahora. Google: copiar el script y pegar la URL de la aplicación web (ver "Sincronizar calendario"). El ID de Microsoft se guarda en `capri_sync.ini`. Intervalo de sincronización de 5, 15 o 30 min.
- **Clima**: detectar mi ubicación (la de Windows o, si no, la aproximada por IP) o una ciudad escrita; con el campo vacío es automática.
- **Sistema**: temperaturas de CPU y GPU, y potencia de la batería, cuando Windows las ofrece.
- **Rendimiento**: animaciones, fotogramas (60 o 30), luz ambiental y degradados, cada cuánto se leen los sensores de Sistema (1, 2 o 5 s) y ahorro con batería (sin animaciones ni luces y a 30 fps al desenchufar). Se aplican al instante.
- **Configuración → Rendimiento → Color de la luz**: elige la luz del pie del panel cuando no suena música: Clásico (azul y dorado), Fondo (dos colores del fondo de escritorio), Windows (el color de acento) o Propio (dos colores que eliges). Solo se recalcula al cambiar el ajuste, el fondo o el acento.
- **IA**: proveedor (OpenAI, DeepSeek, Ollama local o Personalizado), modelo y clave de API. Con Personalizado se editan también el servidor, la ruta, el puerto y HTTPS. Ollama no necesita clave.
- **Acerca de**: versión, memoria en uso, abrir la carpeta de datos y salir.

Desconectar, Borrar y Salir piden confirmación antes de actuar.

La clave de API se guarda cifrada con DPAPI en `%LOCALAPPDATA%\capri_ai_key.bin`, ligada a tu cuenta de Windows. Nunca va al archivo de ajustes ni a los registros.

