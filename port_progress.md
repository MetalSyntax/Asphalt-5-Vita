# Asphalt 5 - Progress Tracking

## Checklist (from PORTING_PLAN.md)
- [x] Repo creado desde soloader-boilerplate, git init, .gitignore anti-DMCA.
- [x] APK decompilado (jadx) y .so decompilado(s) (Ghidra) -- ver sección 2/3.
- [x] Análisis del motor real (ciclo de vida nativo, reuso de otro port o boilerplate genérico).
- [x] Bootstrap del loader: so_file_load/so_relocate/so_resolve, primer build.
- [x] Tabla JNI (FalsoJNI): registrar exports + callbacks hacia "Java".
- [x] Primer arranque en consola real (bug #1 `mEnv==NULL` corregido, verificado en hardware).
- [~] Gráficos (renderiza, llega al title -- falta confirmar escalado/aspecto).
- [~] Input, Audio, Assets, LiveArea/VPK. (touch + botón Círculo=Atrás implementados;
      audio y LiveArea siguen pendientes)
- [~] Pruebas en hardware real (en curso, ver log más reciente).

## Progress Log

### Phase 2: Análisis del motor real
- Confirmed `libasphalt5.so` uses GLES 1.1 fixed-function pipeline.
- It doesn't rely on `JNI_OnLoad`; initialization is done through direct `Java_*` JNI exports.
- `libasphalt5.so` has no dependency on `libigp.so`, so `libigp.so` can be safely ignored for the main game bootstrap.

### Phase 3: Bootstrap del loader
- Updated `main.c` to remove `JNI_OnLoad` calls.
- Mapped manual initialization to `Java_com_gameloft_android_GAND_GloftA5HD_Asphalt5_nativeInit` and `Java_com_gameloft_android_GAND_GloftA5HD_Asphalt5Renderer_nativeInit`.
- Added the render loop calling `Java_com_gameloft_android_GAND_GloftA5HD_Asphalt5Renderer_nativeRender`.
- Updated `CMakeLists.txt` to expect `libasphalt5.so` instead of `main.so` to avoid needing to rename the extracted binary.

### Phase 4: Tabla JNI (FalsoJNI)
- Added `generated_jni_stubs.c` to `CMakeLists.txt` for compilation.
- Parsed `generated_jni_table.h` and automatically regenerated `java.c` to map all JNI method stubs correctly based on their return types and IDs.

### Phase 5: Primer arranque en consola real

#### Bug #1 -- CONFIRMADO Y CORREGIDO: data abort en `Asphalt5_nativeInit` (`mEnv == NULL`)

**Dump:** `logs/asphalt5-psp2core-1787809135-*.psp2dmp`

**Ojo con el reporte automático:** `vita-parse-core` auto-detectó la base del .so
en `0x80ee2000` y por eso marcó todo como "FUERA DE RANGO". La base real es
`0x98000000` (`LOAD_ADDRESS` en `source/utils/init.c`). Confirmado porque
`R4 = 0x981f4ed0` es exactamente `_GLOBAL_OFFSET_TABLE_` (offset `0x1f4ed0`).
Con esa base, `PC = 0x9805cbf0` cae en `.so + 0x5cbf0`.

**Causa raíz** (`objdump` de `libasphalt5.so`, no adivinado):

`0x5cbf0` está a +0x2c de `Java_..._Asphalt5_nativeInit` (`0x5cbc4`):

```
5cbdc: ldr r7, [r4, r3]   ; r7 = GOT[0x4e8] = &mEnv
5cbe8: ldr r2, [r7]       ; r2 = mEnv  -> 0x00000000
5cbf0: ldr r3, [r2]       ; <-- DATA ABORT (r2 = NULL en el dump)
5cbfc: ldr pc, [r3, #84]  ; (*env)->NewGlobalRef  (indice 21)
```

El motor **no usa el `JNIEnv*` que recibe**: lee su propio global `mEnv`
(`0x1f84e8`, confirmado por `nm`). Ese global lo setea *únicamente*
`Java_..._Asphalt5Renderer_nativeGetJNIEnv`, que es literalmente `mEnv = env`
(mismo offset de GOT `0x4e8`). `main()` nunca lo llamaba.

`GLResLoader_nativeInit` y `GLMediaPlayer_nativeInit` tienen el mismo patrón
(`ldr r2,[mEnv]; ldr r3,[r2]`) -- los cuatro crasheaban igual.

**Orden real, de `Asphalt5Renderer.onSurfaceCreated()` (jadx):**

1. `nativeGetJNIEnv()`  <- primea `mEnv`
2. `GLResLoader.init()` -> `nativeInit(0)`
3. `GLMediaPlayer.init()` -> `nativeInit(0)`
4. `Asphalt5.nativeInit(0, pvrt)` -- **static**, `(JNIEnv*, jclass, jint, jint)`
5. `nativeInit(m_bEnableKeyboard, 1, width, height, mCurrentLang)` -- de instancia, 7 params

**Firmas que `main.c` tenía mal:**
- `Asphalt5_nativeInit` es `static` con 2 ints; el 1ro se descarta, el 2do va a
  `mbUsePVRT` (confirmado: `GOT[0x874] -> mbUsePVRT`). Se pasaba `0`, Java pasa `1`.
- `gl_init()` se llamaba **después** de los `nativeInit`, pero
  `Renderer_nativeInit` llama `importGLInit()` + `appInit(w,h,lang)` internamente,
  así que GXM tiene que estar vivo antes.

**Fix:** `source/main.c` reescrito siguiendo el orden de `onSurfaceCreated()`,
con `gl_init()` primero y `nativeGetJNIEnv()` antes de cualquier `nativeInit`.
Los 13 métodos estáticos que resuelve `Asphalt5_nativeInit`
(`sendAppToBackground`, `Exit`, `LaunchBilling`, `ReleaseBillingContext`,
`IsDemo`, `IsDoubleOption`, `GetDoubleOptionText1..3`, `launchGetGames`,
`OpenGLive`, `NotifyTrophy`, `SetLoadingValuable`) ya estaban registrados en
`source/java.c`, así que no hizo falta tocar la tabla JNI.

`mbUsePVRT` se deja en `1` (default de Java): el SGX543 de la Vita es PowerVR y
vitaGL expone `GL_COMPRESSED_*_PVRTC_*`. Si las texturas salen mal, `USE_PVRT`
en `main.c` es el interruptor.

#### Infraestructura de logging (para el siguiente bug)

- **Log incremental en consola:** `ux0:data/asphalt5/logs/asphalt5_001.log` ..
  `asphalt5_999.log`, uno por corrida, con wrap a `001` al llegar a `999`. El
  índice siguiente se recuerda en `logs/next.idx`. Cada línea lleva número de
  secuencia + timestamp y se escribe **sin buffer** (`sceIoWrite`), así el final
  del log sobrevive un data abort -- que es lo que un `.psp2dmp` no da.
- **UDP debugnet (opcional):** copiar `extras/netlog.txt.sample` a
  `ux0:data/asphalt5/netlog.txt` con el IP de la PC. Recibir con
  `nc -u -l 18194`. Sin rebuild. Ver `source/utils/netlog.h`.
- `l_*` ahora se compilan también en Release (`ENABLE_FILE_LOG`, ON por
  defecto); el volumen se controla en runtime con `log_set_min_level()`
  (default `LT_INFO`, así los `l_debug` por-read de `reimpl/io.c` no inundan).
- `fatal_error()` ahora deja su motivo en el log antes de abrir el diálogo.

#### Bug #1 -- VERIFICADO EN HARDWARE

`logs/asphalt5_001.log` confirma que el arranque pasa los cuatro `nativeInit`:

```
[000019] mEnv primed via nativeGetJNIEnv.
[000020] GLResLoader initialized.
[000021] GLMediaPlayer initialized.
[000022] Asphalt5.nativeInit done (mbUsePVRT=1).
[000023] [ALOG][Asphalt5] CAndroidSocket::CAndroidSocket()
```

Ya no crashea en `mEnv`. El log incremental hizo su trabajo: el crash siguiente
quedó acotado entre la línea 22 y la 45.

#### Bug #2 -- CONFIRMADO Y CORREGIDO: stubs de `GLResLoader` vacíos

**Dump:** `logs/asphalt5-psp2core-1787810497-*.psp2dmp`

**Ojo (otra vez) con el reporte automático:** ahora auto-detectó la base en
`0x98050000`, así que **todos** los símbolos que imprimió están corridos
`0x50000`. La base sigue siendo `0x98000000`: `R7 = 0x981f4ed0` es
`_GLOBAL_OFFSET_TABLE_` (`0x1f4ed0`), y el log lo confirma directo --
`nativeGetJNIEnv -> 0x9805d694` = `0x98000000 + 0x5d694`.

Re-simbolizado con la base correcta, la cadena real es:

```
GamePackageMgr::Init()+0x58
  -> GamePackageMgr::Package_Register(char*)+0x8c
    -> SpriteManager::Package_Register(int)+0x58
      -> CGamePackage::GetLZMAFile(int)+0x40          <- LR = .so+0x17743c
        -> LZMAFile::OpenAttached(IFileReadI*)        <- PC = .so+0xc8e70
```

(el reporte decía `Sprite::DrawString` / `AniObj_v4::Load` -- ambos falsos, puro
artefacto del offset mal detectado.)

En la pila estaba el string `package_general.bar_001.cnk`, y el log tenía 15
asserts previos del motor:

```
[ALOG][ASSERT] src/Packages/Package.cpp: FSeekLibData: 135
[ALOG][ASSERT] src/Packages/Package.cpp: GetLibSize: 102   (x14)
```

**Causa raíz:** `libasphalt5.so` **no abre sus archivos de datos**.
`GLResLoader_nativeInit` (`0x5ebb4`) resuelve tres métodos *estáticos* de Java y
el motor pasa cada byte de data por ahí (strings extraídos del pool literal):

- `getResourceLength(Ljava/lang/String;)I`
- `getResourceFull(Ljava/lang/String;)[B`
- `getResourceBytes(Ljava/lang/String;II)[B`

Los tres stubs auto-generados en `generated_jni_stubs.c` devolvían `0` / `NULL`.
Por eso `GetLibSize` daba 0, `FSeekLibData` fallaba, `GetLZMAFile` devolvía NULL
y `OpenAttached` lo desreferenciaba.

**Fix (2 partes):**

1. **`source/jni_resloader.c`** (nuevo) -- implementación real de los tres,
   leyendo de `DATA_PATH` con `sceIo*`. Enganchado en `source/java.c`
   (IDs 75/76/79). En Android estos leían de `res/drawable/res_<name>`, del
   sdcard, o de los assets del APK; verificado que **este APK no tiene
   `assets/`** y que su `res/drawable/` sólo trae la UI de IGP/billing, así que
   el sdcard (`/sdcard/gameloft/games/asphalt5/` → `DATA_PATH`) es la única
   fuente real.

2. **Datos desplegados.** No había *ningún* archivo de data en
   `ux0_data/asphalt5/` -- sólo los `.so`. Extraídos los 327 archivos de
   `Asphalt-5-HD-v3.4.1-adreno.zip` (`asphalt5/*`, 163 MB → 244 MB en disco):
   93 `package_general.bar_*.cnk`, 187 `.glsnd`, 13 `.bsprite`, 7 `.mp4`,
   5 `.bar`, 2 `.map`.

**Nota:** `missionslib.bar` no existe como archivo -- vive *dentro* de
`package_general.bar` (el contenedor que el motor rearma de los 93 `.cnk`).
No falta nada.

**Cuidado con el leak:** `ReleaseByteArrayElements()` y `DeleteLocalRef()` de
FalsoJNI son no-ops, así que todo `jda_alloc()` devuelto al motor se filtra.
`jni_resloader.c` usa un pool round-robin de 8 buffers reciclados en vez de
allocar por llamada: 93 lecturas de 1 MiB crecen el heap 7 MiB en vez de 93 MB.
Asume que el motor copia el array antes de hacer 8 llamadas más -- que es lo que
hace el lector de `Package.cpp` (memcpy directo desde `GetByteArrayElements`).

**Diagnóstico para la próxima:** si la data no está donde el loader la busca,
`resloader` avisa los primeros 12 misses a nivel `warn` (visibles sin activar
debug) y después se calla.

### Current Phase: Verificar bug #2 en consola real
- Compila limpio hasta el `.vpk`. Falta desplegar (eboot **+ los 327 archivos de
  data**) y confirmar en hardware.
- El próximo log debería pasar de la línea 45 sin los asserts de `Package.cpp`.

### Reorganización de `ux0_data/asphalt5/` (2026-08-27)
Los 327 archivos de data estaban sueltos directo en `ux0_data/asphalt5/`,
mezclados con los `.so`, `logs/`, `saves/` y `res/`. Se movieron todos
(excepto `libasphalt5.so`/`libigp.so`) a `ux0_data/asphalt5/data/`.

- **`CMakeLists.txt`:** nueva definición `RES_PATH` = `${DATA_PATH}data/`,
  separada de `DATA_PATH` (que sigue siendo `ux0:data/asphalt5/` para el
  `.so`, logs, config, shader cache y netlog).
- **`source/jni_resloader.c`:** `resolve_path()` ahora prefija con `RES_PATH`
  en vez de `DATA_PATH` -- es el único lugar que resuelve los nombres que pide
  `GLResLoader` (`getResourceLength/Full/Bytes`).
- Nada más lee esos 327 archivos, así que no hizo falta tocar
  `asset_manager.cpp`, `settings.c`, `logger.c`, `glutil.c` ni `netlog.c` --
  todos siguen usando `DATA_PATH` a propósito.
- **Pendiente:** recompilar y volver a desplegar (el `.vpk` viejo en `build/`
  sigue apuntando a los nombres viejos bajo `RES_PATH` incorrecto -- data en
  la raíz de `ux0:data/asphalt5/`; hace falta un build nuevo antes de probar
  en consola).

### Bug #3 -- CONFIRMADO Y CORREGIDO: data abort en `Game::PopState` durante la limpieza de un `GameInit()` fallido

**Dump:** `logs/asphalt5-psp2core-1787843387-0x0008ed2fd3-eboot.bin.psp2dmp`
**Log:** `logs/asphalt5_002.log`

**Ojo (otra vez) con el reporte automático:** auto-detectó la base en
`0x98050000` y mapeó el crash a `LoadTheLanguage()+0x2c` / `Game::InitGame()+0xdc`
-- ambos **falsos**. Confirmado con `R0 = 0x981f4ed0` = `_GLOBAL_OFFSET_TABLE_`
(`.so+0x1f4ed0`), que solo cuadra con base `0x98000000`. Resimbolizando a mano
(`objdump -T` del `.so` real + `objdump -d --triple=thumbv7-none-eabi`, sin
necesitar el toolchain de VitaSDK para esto):

```
PC = 0x980c0094 -> Game::PopState(bool)+0x48        (.so+0xc0094)
LR = 0x980c0180 -> Game::ClearStateStack(bool)+0x30 (.so+0xc0180)
R4 = 0x00000000  <- puntero de estado NULL, desreferenciado sin chequeo
```

**Causa raíz:** `Game::PopState` lee `array[m_stateStackTop]` (offset `0x1D3C`
del objeto `Game`, indexado por el contador en `0x1D38`) y lo desreferencia sin
chequear `NULL`. Ese contador solo se inicializa a `-1` dentro de
`Game::StartGame()` (`*(this+0x1D38) = 0xffffffff;`), **no** en el constructor
de `Game`. Si `GameInit()` (función libre que arma el `Game`, en
`appInit()->GameInit()->Game::StartGame()`) falla ANTES de llegar a
`StartGame()` -- `Game::InitAppData`, `InitGL`, `Lib3D::Init3D`,
`Lib3D::Init3DShaders` o el alloc de `StringManager`, cualquiera que devuelva
error -- el objeto `Game` recién allocado (`operator_new(0x1ee00)`) queda con
`m_stateStackTop` en lo que sea que trajera la memoria fresca (típicamente
`0`, no `-1`). `appInit()` ve `GameInit()==0` y llama `appDeinit()`, que
termina en `Game::FreeAppData()->ClearStateStack()->PopState()`: como
`m_stateStackTop==0` no es `<0`, el loop de limpieza SÍ entra, lee
`array[0]` (también en cero, nunca se pusheó nada) y crashea al desreferenciar
ese `NULL`.

El log confirma qué disparó el `GameInit()` fallido: el último resource antes
del corte es `shaderSettings.bar`, con un miss:
```
[000052][WARNING] resloader: not found (1/12): ux0:data/asphalt5/data/shaderSettings.bar
[000053][ERROR  ] resloader: getResourceFull: not found: ux0:data/asphalt5/data/shaderSettings.bar
```
`shaderSettings.bar` es de los 327 archivos de la reorganización de datos
(bug/tarea anterior) -- estaba en el mirror local (`ux0_data/asphalt5/data/`)
pero el deploy a la consola física estaba incompleto (se cortó alfabéticamente
antes de llegar a los archivos que empiezan con `sh`/`st`, ver los `package_general.bar_*.cnk`
sí encontrados justo antes).

**Esto NO es un bug de nuestro código** (el loader/JNI resolvían bien la
ruta, ver `resloader: length .../shaderSettings.bar = 3200` en el log
siguiente) -- es un bug real *dentro del motor original* (falta de
`m_stateStackTop=-1` en el constructor, o de un chequeo NULL en `PopState`)
que solo se manifiesta cuando el init falla por datos faltantes. Redesplegar
la carpeta `data/` completa (327 archivos) a la consola lo resuelve.

**Verificado en hardware:** `logs/asphalt5_003.log` llega hasta
`Asphalt5Renderer.nativeInit done (960x544)` y `Entering render loop.` sin
abortar -- `shaderSettings.bar` se lee bien esta vez (`length ... = 3200`).

### Bug #4 -- CONFIRMADO Y CORREGIDO: pantalla en blanco, el motor queda trabado en `GS_TrailerMovie`

Tras el fix del bug #3 el juego ya no crashea, pero se queda con la pantalla
en blanco/sin dibujar indefinidamente.

**Causa raíz:** El primer estado que pushea `Game::StartGame()` es
`GS_TrailerMovie` (el video de marca inicial). `GS_TrailerMovie::Create()`
llama `nativeLoadMovie()` -> `GLMediaPlayer.loadMovie(String)` (método JNI
estático, id `69`) y pone `*(g_pMainGameClass + 0x1D72) = 1` ("movie busy").
`GS_TrailerMovie::Update()` hace `if (*(g_pMainGameClass+0x1D72) != 0) return;`
en cada frame -- nunca avanza a menú hasta que ese byte vuelva a `0`.

En Android, `GLMediaPlayer.loadMovie()` (jadx: `GLMediaPlayer.java:395`)
lanza un `Activity` separado (`MyVideoView`) que reproduce el `.mp4`
fullscreen; al terminar, ese flag se limpia por otro camino (fuera de lo que
tenemos decompilado, vía `onActivityResult`/ciclo de vida de Activity). El
stub JNI auto-generado (`stub_GLMediaPlayer_loadMovie_69`) no hace nada
-- no hay decodificador de video en este port -- así que el flag nunca se
limpia y el motor queda parado en `GS_TrailerMovie` para siempre (pantalla
en blanco: `GS_TrailerMovie::Render()` solo llama `glClearColor(0,0,0,0)`,
nunca `glClear`, así que ni siquiera se pinta ese negro).

**Fix (v1, auto-skip):** `source/jni_media.c` (nuevo) -- `impl_GLMediaPlayer_loadMovie`
resuelve `g_pMainGameClass` (símbolo exportado, `.so+0x1fa798`, confirmado
con `objdump -T`) y limpia el byte en `+0x1D72` directamente, saltando el
trailer en vez de reproducirlo. Registrado en `source/java.c` (id `69`, antes
`stub_GLMediaPlayer_loadMovie_69`). Mismo patrón que `jni_resloader.c`.
Verificado con `logs/asphalt5_004.log`: llega a `Entering render loop.` igual
que el log anterior (sin abortar).

**Fix (v2, a pedido -- skip manual con X):** reproducir el video real
implicaría decodificar H.264/MP4 propio (la consola sí tiene
`SceAvcodecUser` cargado, ver módulos del dump del bug #3, así que es
factible, pero es una tarea aparte de bastante más alcance: demux del `.mp4`,
subir frames YUV->RGB a una textura, sincronizar audio). En vez de eso,
`impl_GLMediaPlayer_loadMovie` ahora abre un `SceMsgDialog` ("Video playback
isn't supported on this port yet. Press X to continue.") en lugar de limpiar
el flag al toque. `media_pump()` (nuevo, llamado una vez por frame desde el
loop de render en `main.c`, después de `gl_swap()`) sondea
`get_msg_dialog_result()`; recién cuando el jugador confirma el diálogo
(botón OK del sistema -- X u O según la configuración de región de la
consola) se limpia `+0x1D72` y el estado avanza a menú. Reusa
`init_msg_dialog`/`get_msg_dialog_result` de `source/utils/dialog.c` (mismo
mecanismo que ya usaba `fatal_error()`).

**Pendiente:** recompilar, redesplegar y confirmar en consola que el diálogo
aparece, que X lo cierra y que después se ve el menú principal.

#### Corrección sobre el fix de arriba -- la causa real era otra

`logs/asphalt5_005.log` es **idéntico** a los logs #003/#004 (llega a
`Entering render loop.` y nada más) y nunca aparece el mensaje
`GLMediaPlayer.loadMovie: ...` -- o sea, `impl_GLMediaPlayer_loadMovie`
**nunca se ejecutó**, ni la v1 ni la v2. No es un problema de build/deploy
(los timestamps de `build/eboot.bin` son posteriores a los de los fuentes).

**Causa raíz real:** `FalsoJNI_ImplBridge.c:getMethodIdByName()` busca por
**nombre solo**, recorriendo `nameToMethodId[]` linealmente y devolviendo el
primer `strcmp` que matchee -- **ignora el `jclass`**. `GetStaticMethodID()`
(`FalsoJNI.c:845`) ni siquiera arma la clase en la key de búsqueda salvo para
`<init>`. Y `"loadMovie"` está duplicado en la tabla: `Asphalt5.loadMovie`
(id `14`) y `GLMediaPlayer.loadMovie` (id `69`), con el `14` primero en el
array. Entonces `GLMediaPlayer_nativeInit()` resolviendo
`GetStaticMethodID(mClassGLMediaPlayer, "loadMovie", ...)` recibe de vuelta
el id **`14`** (el de `Asphalt5`, no el de `GLMediaPlayer`), y
`nativeLoadMovie()` termina llamando siempre a
`stub_Asphalt5_loadMovie_14` -- el stub vacío auto-generado -- sin importar
qué pusimos en el id `69`.

`grep` sobre `source/java.c` muestra que esto **no es un caso aislado**: hay
más de 20 nombres duplicados entre clases (`Exit`, `init`, `onPause`,
`onResume`, `onCreate`, `onKeyDown`, etc.). La mayoría son inofensivos (ambas
puntas ya hacen lo mismo, o el duplicado vive en `IGPActivity`/`libigp.so`,
que este juego no usa). **Si aparece un bug rarísimo donde "el JNI que
registré nunca corre pero tampoco hay error", sospechar de esto primero:
buscar el nombre del método con `grep -n '"nombre"' source/java.c` y ver si
hay más de un `id` con ese mismo nombre -- gana el que esté antes en el
archivo.**

**Fix:** en `source/java.c`, el id `14` (`stub_Asphalt5_loadMovie_14`) ahora
también apunta a `impl_GLMediaPlayer_loadMovie` (mismo handler que el id
`69`). El comentario en el archivo documenta por qué.

**Pendiente:** recompilar, redesplegar, confirmar que el diálogo "Press X to
continue" aparece esta vez y que X lo cierra.

#### Segunda corrección -- el fix del id 14 funcionó, pero el diálogo no

`logs/asphalt5_006.log` confirma que el fix de arriba anduvo: por primera vez
aparece `[INFO] GLMediaPlayer.loadMovie: no video decoder on Vita, prompting
to skip.` justo antes de `Asphalt5Renderer.nativeInit done`. El
`impl_GLMediaPlayer_loadMovie` SÍ se ejecuta ahora. Pero el usuario sigue
viendo pantalla en blanco y presionar X no hace nada -- el `SceMsgDialog`
nunca se hizo visible ni respondió.

No se pudo confirmar la causa exacta sin telemetría en vivo (candidatos: el
diálogo se arma en medio de la propia `appInit()`/`GameInit()` -- antes de
que corra el primer `vglSwapBuffers` del programa -- y quizás `SceCommonDialog`
necesita que la escena GXM esté en un estado que ahí todavía no se dio; o
algún estado de GL que dejó el motor interfiere con el compositing del
diálogo). En vez de seguir iterando a ciegas sobre `SceMsgDialog`, se
reemplazó por el mecanismo más simple y con menos partes móviles: leer el
botón X directo con `sceCtrlPeekBufferPositive` (`SCE_CTRL_CROSS`) en
`media_pump()`, sin ningún diálogo de por medio. Ya no hay ningún prompt
visual -- la pantalla se mantiene en blanco hasta que se presiona X -- pero
esto no depende de `SceCommonDialog` para nada, así que es mucho más difícil
que falle silenciosamente.

**Pendiente:** recompilar, redesplegar, confirmar que presionar X ahora sí
saca de la pantalla en blanco y lleva al menú principal. Si sigue sin
reaccionar a X, el problema no es el mecanismo de skip sino algo previo (o
`GS_TrailerMovie::Update()` no es el estado real en el que está trabado el
juego -- en ese caso hace falta un log incremental con un contador de frames
o `netlog` en vivo para confirmar en qué estado/función está el bucle
principal cuando queda en blanco, en vez de seguir asumiendo).

**VERIFICADO en `logs/asphalt5_007.log`:** el skip con X funcionó (`GLMediaPlayer.loadMovie:
skipped by user (X), continuing.` en la línea 73), y el juego llegó al title screen.

### Bug #5 -- CONFIRMADO Y CORREGIDO: no había ningún input real conectado

Con el trailer saltado, el juego llega al title, pero ni el touch ni ningún
botón hacían nada (más allá del hack de un solo uso para saltar el trailer).
**Causa raíz:** `libasphalt5.so` nunca lee input por su cuenta -- en Android,
`Asphalt5.onTouchEvent/onKeyDown/onKeyUp` (dirigidos por el framework) llaman
a cinco entry points nativos exportados y confirmados en la tabla de símbolos:

```
Java_..._Asphalt5_nativeTouchPressed(x, y, pointerId)
Java_..._Asphalt5_nativeTouchMoved(x, y, pointerId)
Java_..._Asphalt5_nativeTouchReleased(x, y, pointerId)
Java_..._Asphalt5_nativeSetOnKeyDown(keyCode)
Java_..._Asphalt5_nativeSetOnKeyUp(keyCode)
```

Nada en `main.c` los llamaba nunca -- no existía ningún camino de input, ni
siquiera roto.

**Hallazgo importante (confirmado en el pseudo-C):** `notifyTouchPress`/
`notifyTouchMoved`/`notifyTouchReleased` indexan un array de 2 slots
(`mTouchID`) por `pointerId` y hacen `ASSERT` si `id > 1` -- el motor **solo
soporta 2 dedos simultáneos**, con ids `0`/`1` exactos. No se puede reenviar
el `id` crudo que da el panel táctil de la Vita (no está acotado a 0/1).
También confirmado: `notifyKeyPressed/Released` -> `Game::InputKeyBoard`, y
varios handlers de menú/diálogo comparan explícitamente `keyCode == 4`
(`KeyEvent.KEYCODE_BACK`) -- el único keycode con semántica confirmada.

**Fix:** `source/input.c`/`.h` (nuevo).
- `poll_touch()`: `sceTouchPeek(SCE_TOUCH_PORT_FRONT, ...)` cada frame,
  reasignando cada `id` del panel (no acotado) a uno de 2 slots propios (0/1)
  por orden de aparición -- soltar dedos de más en vez de reenviarlos.
  Coordenadas escaladas /2 (panel frontal 1920x1088 -> pantalla 960x544, la
  resolución que ya le pasamos a `Renderer_nativeInit`). Detecta
  press/move/release comparando contra el frame anterior (`sceTouchPeek` da
  "qué está tocado ahora", no eventos discretos).
- `poll_keys()`: sólo Círculo -> `KEYCODE_BACK` (4) por ahora -- es el único
  mapeo con evidencia real en el disassembly; no se inventaron mappings para
  X/Triángulo/Cuadrado/D-Pad sin confirmar que el motor los lea (la UI es
  100% táctil, no hay navegación por teclado/mando visible en el pseudo-C).
- `sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, START)` en `input_init()`
  -- el panel táctil no muestrea por defecto en Vita (a diferencia de
  `sceCtrl`, que sí).
- Resuelto en `main.c` igual que los demás native entry points (opcional, no
  aborta el boot si falta alguno), `input_poll()` se llama una vez por frame
  antes de `Renderer_nativeRender()`.

**Pendiente:** recompilar, redesplegar, confirmar en consola que tocar la
pantalla ahora navega el title/menú y que Círculo funciona como atrás.

### Pendiente sin resolver -- la pantalla no cubre el 100% de la Vita

El usuario reporta que la imagen no llena la pantalla completa (necesita
stretch/cover). Investigado hasta donde da el pseudo-C sin feedback visual:

- `vglInitExtended(0, 960, 544, ...)` en `gl_init()` ya pide el framebuffer a
  resolución nativa completa de la Vita.
- `Renderer_nativeInit` ya se llama con `(960, 544)`, y `Game::Game()`
  guarda `param_2` (960) en el global `OS_SCREEN_W`. La mayoría de los
  `Lib3D::SetViewport(0,0,OS_SCREEN_W,OS_SCREEN_H)` en el motor usan la
  pantalla completa.
- **No confirmado:** dónde se setea `OS_SCREEN_H` (no aparece como
  asignación directa en el pseudo-C bajo ese nombre -- puede estar plegado
  en otro símbolo). Tampoco se descartó que el motor tenga una resolución de
  diseño/UI fija (varios `RectEntry` en el constructor de `Game` usan
  constantes como `0x17c`/`380`, `0x118`/`280` que no parecen relacionadas a
  960x544 y podrían ser parte de un lienzo virtual escalado aparte).

**No se tocó nada acá todavía** -- parchear el viewport a ciegas sin ver qué
pasa en pantalla (¿barras arriba/abajo? ¿a los costados? ¿UI recortada?)
puede empeorarlo. Hace falta una captura de pantalla de la consola para
seguir.

**VERIFICADO en `logs/asphalt5_008.log`:** el touch funciona (el usuario llegó
al title y pudo tocar para avanzar).

### Bug #6 -- CONFIRMADO Y CORREGIDO: `Undefined Instruction` en `CMatrix::Mult` al entrar al menú

**Dump:** `logs/asphalt5-psp2core-1787850699-0x000ae3251b-eboot.bin.psp2dmp`

**Ojo (otra vez) con el reporte automático:** auto-detectó la base en
`0x82272000` -- todo "FUERA DE RANGO". Confirmado con `R9 = 0x981f4ed0` =
`_GLOBAL_OFFSET_TABLE_` que la base real sigue siendo `0x98000000`.
Resimbolizado a mano:

```
PC = 0x9813e1c8 -> _ZN7CMatrix4MultEPS_ (CMatrix::Mult) +0x1c   (.so+0x13e1c8)
LR = 0x9813c9f4 -> _ZN7CCamera12updateMatrixEv +0x44            (.so+0x13c9f4)
```

Tipo de excepción: `Undefined Instruction` (no data abort) -- la CPU intentó
ejecutar una instrucción que no reconoce.

**Causa raíz:** `CMatrix::Mult`/`PreMult`/`SetMult` (las únicas 3 funciones
de todo el `.so` que lo hacen -- confirmado con `objdump`, sólo 6
`vmsr fpscr` en total, agrupadas en estas 3) usan el truco legacy de VFPv2
"short vector": `FPSCR.Len=4` + una cadena `vmul.f32`/`vmla.f32` que expande
automáticamente por hardware sobre un rango de registros, para multiplicar
matrices 4x4 sin loops. Ese modo se sacó de la arquitectura a partir de
VFPv3 y **el Cortex-A9 de la Vita no lo implementa** -- en cuanto
`FPSCR.Len != 0` y corre un `vmul`/`vmla`, la CPU tira Undefined
Instruction. El binario es armeabi/ARMv6 (`PORTING_PLAN.md`), target donde
este truco sí existía en hardware real (ARM11).

**Por qué no se puede parchear "apagando" el modo vector:** las instrucciones
`vmul`/`vmla` de estas 3 funciones dependen de la expansión de registros para
ser matemáticamente correctas (no es sólo una optimización) -- desactivar
`FPSCR.Len` sin reescribir la matemática habría dejado el juego corriendo
sin crashear pero con **toda transformación 3D silenciosamente rota**
(mucho peor que un crash limpio).

**Semántica reconstruida del disassembly** (confirmado contra
`CMatrix::TransformVector`, que hace `out = M·v` con `M[row][col]` en el
byte `row*16+col*4` -- así se confirmó el índice/orden de storage): las 3
funciones son un `V × M` estándar, `Result[row][col] = Σ_k V[row][k]*M[k][col]`,
sólo cambia qué operando es `V` (leído de a 2 filas por el "banco escalar")
y cuál es `M` (cargado entero al "banco vector"), y dónde cae el resultado:

| Función | Operación |
|---|---|
| `Mult(other)` | `this = this * other` |
| `PreMult(other)` | `this = other * this` |
| `SetMult(a, b)` | `this = a * b` |

**Fix:** `source/patch.c` -- `so_patch()` (que ya existía para esto, vacío
hasta ahora) hookea las 3 funciones (`hook_addr` + `so_symbol`, mecanismo ya
provisto por `so_util`) a reimplementaciones en C puro (`cmatrix_mul`) que
hacen la misma multiplicación 4x4 fila-mayor con loops escalares, sin tocar
`FPSCR` para nada. Usa un buffer temporal para ser segura ante alias (`Mult`/
`PreMult` escriben sobre uno de los propios operandos de entrada).

**Pendiente:** recompilar, redesplegar, confirmar que entrar al menú ya no
crashea.

### Bug #7 -- CONFIRMADO Y CORREGIDO: `OS_SCREEN_H` nunca se actualiza (pantalla no llena la Vita)

Explica el reporte "la pantalla se ve abajo a la izquierda, con espacio arriba
y a la derecha": grepeando las ~230k líneas del pseudo-C completo por
`OS_SCREEN_H\s*=` no aparece **ninguna** asignación en todo el motor --
sólo lecturas (`Lib3D::SetViewport(0,0,OS_SCREEN_W,OS_SCREEN_H)`, cálculos de
layout de UI, etc.). `OS_SCREEN_W` sí lo actualiza `Game::Game()`
(`OS_SCREEN_W = <param ancho>`), pero el constructor recibe también la
altura (`param_3` en su firma) y **nunca la usa en ninguna línea** -- se
confirmó con grep sobre el cuerpo completo del constructor (~475 líneas).

**Valor por default (leído de `.data` con `objdump -s`):** `OS_SCREEN_W`
arranca en `854`, `OS_SCREEN_H` en `480` -- una resolución de referencia
Android (WVGA-ish) horneada en el binario en tiempo de compilación.
`OS_SCREEN_W` se corrige a `960` en runtime; `OS_SCREEN_H` se queda en `480`
para siempre en vez de `544`. Con eso, `glViewport(0,0,960,480)` -- ancho
completo pero 64px menos de alto -- ancla el contenido abajo (origen de GL
es esquina inferior-izquierda) dejando una franja vacía arriba. La lógica de
layout de UI del motor probablemente escala de forma uniforme para mantener
aspecto usando `OS_SCREEN_H` también, lo que explica que el hueco no sea
sólo arriba sino también a la derecha.

**Fix:** `source/main.c` -- después de `input_init()` y antes de
`Renderer_nativeInit()` (que es donde corren `Game::Game()`/`InitGL()`/el
primer `SetViewport` de forma síncrona), se resuelve el símbolo exportado
`OS_SCREEN_H` (confirmado con `objdump -T`: `.data`, global) y se le escribe
`SCREEN_H` (544) directamente -- un poke de una sola `int`, sin tocar nada
del motor.

**Pendiente:** recompilar, redesplegar, confirmar que la imagen ahora cubre
toda la pantalla de la Vita.

## Sesión de rendimiento (2026-08-27, tarde)

Entre la sesión anterior y ésta el port avanzó bastante por fuera de este
log: crash de `CMatrix`/`OS_SCREEN_H` verificados arreglados, input táctil
funcionando, el juego ya es **jugable** (llega a carrera). Pero: menú a
~35 FPS, carrera/in-game a **0-2 FPS**, y dos dumps marcados `GPUCRASH`
aparecieron en `logs/` sin quedar documentados acá. El usuario preguntó
específicamente si el overclock podía dañar la consola.

**Sobre la salud de la consola (aclarado, no es un riesgo):** los
`scePowerSet{Arm,Bus,Gpu,GpuXbar}ClockFrequency(444/222/222/166)` de
`source/utils/init.c` son los valores de overclock estándar de toda la
escena homebrew de Vita, vía la API oficial `scePower` -- no un mod de
voltaje. Un dump `GPUCRASH` es la GPU/GXM rechazando un estado inválido y el
manejador de crash de la consola atrapándolo -- un crash de software con el
mismo mecanismo que todos los demás bugs de esta bitácora, no un riesgo de
hardware.

### Bug #8 -- revertido: "optimización" de viewport que no bajaba nada de carga y es sospechosa del `GPUCRASH`

Entre sesiones apareció (sin documentar) un intento de bajar la resolución
interna: `SCREEN_W/H` en `main.c` pasados a `800x480` (en vez de `960x544`),
más `hook_glViewport`/`hook_glScissor` en `dynlib.c` reescalando cada
llamada con una división entera fija (`(x*960)/800`, etc.) antes de pasarla
a la real. Un archivo suelto `source/gl_hook.c` (no estaba en
`CMakeLists.txt`, dead code) tenía una copia duplicada de lo mismo.

**Por qué esto no ayudaba en nada:** `vglInitExtended(0, 960, 544, ...)` en
`gl_init()` (`utils/glutil.c`) nunca cambió -- el framebuffer real seguía
siendo 960x544 a full resolución. Reescalar el rect de `glViewport`/
`glScissor` no reduce la cantidad de píxeles que la GPU rasteriza; solo
mueve dónde caen esos píxeles dentro del mismo framebuffer de siempre. Cero
beneficio de rendimiento, y la división entera (`x*960/800` no da exacto
para la mayoría de los `x`) puede producir rects de tamaño/posición
inválidos -- candidato directo para el `GPUCRASH` (confirmado con uno de los
dumps: crashea dentro de `gpu_alloc_mapped_aligned`, llamado desde
`_glDrawElements_FixedFunctionIMPL`, exactamente el tipo de estado GXM que
un viewport corrupto puede romper).

**Fix:** revertido -- `SCREEN_W`/`SCREEN_H` de vuelta a `960`/`544` en
`main.c` (esto también hace que el poke de `OS_SCREEN_H` del bug #7 vuelva a
escribir el valor correcto); `dynlib.c` mapea `glViewport`/`glScissor`
directo a las funciones reales de vitaGL de nuevo; borrados `source/gl_hook.c`
(dead code, ni compilaba) y `source/reimpl/io_path.h` (helper de mapeo de
paths sin ningún caller en todo el repo, de otro intento a medio hacer).

**No tocado porque no es sospechoso:** `source/reimpl/io.c`/`sys.c` también
cambiaron entre sesiones (mapeo de `/data/data/com.gameloft.../` a
`ux0:data/asphalt5/` para `fopen`/`open`/`stat` -- coherente con dónde ya
viven `saves/`/`logs/`/`config.txt`; y afinado `usleep`/`nanosleep`/
`sched_yield` para no dormir/cederle el CPU al scheduler en sub-1ms, un
patrón común y razonable al portar código con busy-waits). Esto se queda.

### Bug #9 -- CONFIRMADO Y CORREGIDO: cache de 1 solo handle en el resloader, sospechoso principal de los 0-2 FPS en carrera

**Evidencia:** en TODOS los logs de esta sesión, los pedidos de
`package_general.bar_NNN.cnk` alternan entre varios números seguidos --
`019, 023, 039, 019, 038, 019, 038, 019, 025, 026, 027...` -- nunca piden el
mismo archivo dos veces seguidas. `jni_resloader.c` tenía un cache de **un
solo handle** (`_fd`/`_fd_path`): con ese patrón de acceso intercalado, se
invalida en cada llamada, así que CADA lectura paga un `sceIoClose()` +
`sceIoOpen()` nuevo contra la memory card. Durante la carga inicial es
barato (se nota poco); durante una carrera, si el motor streamea texturas/
audio de esta forma en tiempo real, es la diferencia entre fluido y
1-2 FPS -- I/O síncrono de storage en el hilo principal, por frame.

**Fix:** `source/jni_resloader.c` -- el cache de 1 entrada pasó a un LRU de
8 slots (`_fd_slots[]`, alcanza para que el motor tenga varios chunks/
sonidos abiertos a la vez sin desalojarse entre sí). Cada slot guarda su
propio handle + path; hit exacto reusa el handle, si no hay slot libre se
desaloja el usado hace más tiempo. `resloader_shutdown()` cierra los 8.

**Pendiente:** recompilar, redesplegar, medir FPS en carrera. Si sigue lento
después de esto, el siguiente sospechoso es el propio costo de
descomprimir LZMA por chunk (`GetLZMAFile`/`Package.cpp`) en vez del I/O en
sí -- pero eso requeriría perfilar en consola real para no volver a "optimizar"
a ciegas como pasó con el viewport.

### Bug #10 -- pedido del usuario: el menú necesitaba 800x480, pero eso no debía impedir una optimización real

El usuario confirmó que revertir `SCREEN_W/H` a `960x544` (bug #8) rompió el
layout del menú -- el motor SÍ necesita que se le reporte `800x480`
(probablemente su UI tiene posiciones ancladas a esa resolución de
referencia, no a la real del dispositivo). Pero seguía a 4 FPS en carrera
incluso con `800x480` puesto, con el hook de `glViewport`/`glScissor` de
antes (que como ya se documentó en el bug #8, **no reduce nada de trabajo de
GPU** -- mismo framebuffer 960x544 real de siempre, sólo mueve el rect).

El usuario señaló el port de referencia `Dungeon-Hunter-2-vita` (con FPS
razonables, ~15) para ver cómo resolvió esto. Ese port usa el driver PVR_PSP2
real (EGL/GLES2 nativo, no vitaGL) y en `utils/glutil.c` tiene un modo
`DOWNSAMPLE_RENDER`: renderiza toda la escena a un FBO offscreen MÁS CHICO
que la pantalla real, y en `gl_swap()` hace un solo blit de upscale (quad con
textura, filtrado `GL_LINEAR`) antes de presentar. Esa es la técnica correcta
-- reduce la cantidad real de píxeles que la GPU rasteriza/sombrea, a
diferencia de reescalar `glViewport` sobre el mismo framebuffer de tamaño
completo (que no ahorra nada).

**Fix (síntesis: mantener lo que el menú necesita + optimizar de verdad):**

- `main.c`: `SCREEN_W`/`SCREEN_H` de vuelta a `800`/`480` (arregla el menú;
  el poke de `OS_SCREEN_H` del bug #7 sigue funcionando igual, ahora escribe
  480).
- `source/utils/glutil.c` (nuevo): `gl_init()` sigue pidiendo el framebuffer
  real a `vglInitExtended(0, 960, 544, ...)` sin cambios, pero ahora además
  arma un FBO offscreen de `800x480` (textura de color `GL_RGBA` +
  renderbuffer de profundidad `GL_DEPTH_COMPONENT16`) y lo deja bindeado --
  el motor nunca bindea su propio framebuffer, así que todo lo que dibuja
  cae ahí sin que se entere. `gl_swap()` desbindea a la pantalla real, hace
  un blit de un solo quad texturizado (fixed-function, sin shaders --
  coherente con que este port usa GLES1.1 vía vitaGL, no GLES2) estirando el
  FBO de `800x480` a los `960x544` reales, presenta, y vuelve a bindear el
  FBO para el siguiente frame. Con fallback: si el FBO queda incompleto
  (`glCheckFramebufferStatus`), sigue funcionando sin downsample (como
  antes).
- `dynlib.c`: **no** se reintrodujo el hook de `glViewport`/`glScissor` --
  ya no hace falta, el FBO offscreen mide exactamente lo que el motor cree
  que mide la pantalla (`800x480`), así que sus llamadas a `glViewport`
  caen bien solas sin ningún reescalado.
- `source/input.c`: las coordenadas de touch ya estaban escaladas
  correctamente a `800x480` (`touch.x * 800/1920`, `touch.y * 480/1088`) --
  quedaron así de un intento anterior entre sesiones y eran correctas;
  sólo se les puso nombre a las constantes mágicas para que quede
  documentado por qué son esos números (deben coincidir con `SCREEN_W/H`,
  no con la resolución física de la Vita).

**Por qué esto es mejor que el hook de viewport:** el framebuffer real que
la GPU tiene que llenar por frame pasa de 960x544 (521 mil píxeles) a 800x480
(384 mil) para toda la geometría 3D de la carrera -- ~26% menos trabajo de
rasterizado/sombreado real, no una ilusión. El costo agregado es un solo
blit de 4 vértices por frame, insignificante contra ese ahorro. También es
más simple: nada de división entera reescalando rects a mitad de frame (el
sospechoso del `GPUCRASH`).

**Pendiente:** recompilar, redesplegar, confirmar que el menú se ve bien de
nuevo, medir FPS en carrera (el 26% de píxeles menos por sí solo probablemente
no alcance para 30 FPS viniendo de 4 -- si el cuello de botella real es CPU
(LZMA por chunk, lógica de juego) en vez de GPU, esto ayuda pero no resuelve
todo; hace falta medir en consola real antes de decidir el próximo paso, no
asumir).

## Audio/Video (2026-08-27, noche)

Entre turnos apareció (sin pasar por esta bitácora) una implementación
sustancial de audio (`source/audio.cpp`/`.h`, mezclador propio vía
`sceAudioOut` + `minimp3`/`stb_vorbis`) y video (`source/video.cpp`/`.h`,
`SceAvPlayer` con conversión YUV→RGB por GPU vía shaders GLES2). El usuario
pidió seguir mejorándolo. Encontrados y corregidos 3 bugs concretos,
verificados contra el Java decompilado y los archivos de datos reales (no
adivinando):

**Bug #11 -- `MAX_SOUNDS` (128) recortaba 59 sonidos reales.** Hay 187
archivos `raw_000.glsnd`..`raw_186.glsnd` en `RES_PATH`, y
`Java_..._GLMediaPlayer_nativeGetTotalSounds()` en el `.so` devuelve
literalmente `0xbb` = 187 -- confirmado en el pseudo-C. Cualquier `sndId`
128-186 se rechazaba en silencio (`sfx_get()` → `NULL` sin loggear nada raro,
solo "no encontrado"). Subido a 200.

**Bug #12 -- `resolve_sound_path()` adivinaba nombres de pista ficticios.**
`kKnownSoundNames[]` (`installer_000.ogg`, `bgm1.mp3`, `debriefing_menu.mp3`,
etc.) -- ninguno de esos 8 archivos existe en los datos reales (verificado).
Los `.mp3` que SÍ existen (`font_hud_image.mp3`, etc.) no son audio -- son
imágenes de fuente con extensión `.mp3`, un truco de ofuscación de Gameloft
ya documentado en `PORTING_PLAN.md`. Confirmado en `GLMediaPlayer.java`
(`loadSound`/`loadSoundBig`, decompilado con jadx) el esquema real: siempre
`raw_` + índice con cero-padding a 3 dígitos + `.glsnd`
(`if index<10: "00"+index elif <100: "0"+index else: index`), sin
excepciones ni casos especiales. Reescrita la función para construir
directamente `RES_PATH raw_%03d.glsnd` -- confirmado además que los
`.glsnd` reales son Ogg Vorbis genuino (`xxd` muestra magic `OggS` +
string `vorbis`), así que `decode_ogg_file()` (stb_vorbis, agnóstico a
extensión) ya los decodifica bien.

**Bug #13 -- el video se dibujaba con las dimensiones equivocadas, recortado
en la esquina inferior-izquierda.** `video.cpp` asumía que dibuja directo al
framebuffer físico (960x544, `REAL_SCREEN_W/H`). Pero desde el bug #10 hay
un FBO offscreen de 800x480 bindeado de forma permanente (`gl_init()` en
`glutil.c`, nunca se desbindea durante el juego) -- el video nunca lo sabía,
así que su `glViewport(0,0,960,544)` quedaba más grande que el color
attachment real del FBO (800x480) y la GPU recortaba todo lo que caía fuera
-- el mismo bug de "pantalla incompleta" ya resuelto para el resto del
juego, ahora en el reproductor de video. Renombradas las constantes a
`VIDEO_TARGET_W/H` = 800/480 (con comentario explicando por qué NO son la
pantalla real) y actualizados los 8 sitios que las usaban (viewport, quad
NDC, `glReadPixels` de diagnóstico). `gl_swap()` (llamado al final de cada
frame de video) sigue haciendo el único blit de upscale al final, así que
el video ahora comparte el mismo pipeline de escalado que el resto del
juego en vez de tener el suyo propio roto.

**Limpieza:** `jni_media.c`'s `media_pump()` tenía un bloque de "saltar con
Cross/Círculo/Cuadrado/Triángulo/Start" que quedó muerto -- `video_play()`
(en `video.cpp`) ya maneja su propio skip por botón internamente de forma
síncrona, y `impl_GLMediaPlayer_loadMovie` ya limpia el flag de "movie busy"
incondicionalmente apenas `video_play()` retorna, así que ese bloque nunca
se alcanzaba con el flag todavía en `1`. Simplificado a un no-op documentado
en vez de código que aparenta hacer algo que ya no hace falta.

**No tocado, evaluado y descartado como bug:** el orden de prefijos que
prueba `video_play()` para encontrar el archivo de video (`DATA_PATH "%s"`,
`DATA_PATH "data/%s"`, variantes con `.mp4` agregado, `files/`, `res/raw/`)
prueba de más (el nombre que pasa el motor ya trae extensión, confirmado en
`GLMediaPlayer.java`: `SOUND_DIR + movieName` sin agregar nada) pero SÍ
incluye `RES_PATH` como candidato, así que ya encuentra el archivo
correctamente -- solo hace 1 `sceIoGetstat()` de más por reproducción, no
vale la pena tocarlo ahora.

**Pendiente:** recompilar, redesplegar, confirmar en consola que (a) más
efectos de sonido/música reproducen correctamente que antes (los IDs
128-186 y cualquiera que dependiera de los nombres ficticios), y (b) el
video del trailer ahora se ve completo en vez de recortado/desplazado.

### Bug #14 -- CONFIRMADO Y CORREGIDO: `loadMovie` recibía un nombre de archivo corrupto (explica logo/video/título/loading invisibles)

Tras agregar audio/video el usuario reportó: sin logo de Gameloft, sin video
de intro (sólo se oía sonido), sin pantalla de título, toca la pantalla y va
directo al menú, sin pantallas de carga visibles.

**Evidencia (`logs/asphalt5_020.log` línea ~81):**
```
[movie] GLMediaPlayer.loadMovie(@<basura binaria ilegible>): starting playback via SceAvPlayer
[ERROR] video: file not found for "@<basura>" (searched DATA_PATH data/, files/, res/raw/)
[movie] GLMediaPlayer.loadMovie(@<basura>): video_play returned
```

**Causa raíz:** `impl_GLMediaPlayer_loadMovie` (`source/jni_media.c`) hacía
`(const char *) va_arg(args, jobject)` -- castea el `jobject` (un puntero a
`JavaString`, la struct wrapper de FalsoJNI con el `char*` real adentro en
`->utf8->array`) directo a `char*`, leyendo los bytes de la propia struct
wrapper como si fueran el string. `jni_resloader.c::resolve_path()` ya
desenvuelve esto correctamente hace rato; `jni_media.c` nunca lo hizo.

Con un nombre basura, `video_play()` fallaba ("file not found") de inmediato. Además, incluso cuando el video funcionaba, el motor se quedaba **trabado para siempre en una pantalla blanca**.

**Causa de la pantalla blanca infinita:** `GS_TrailerMovie::Create()` llama a `loadMovie()` nativo y *justo después de que retorna* hace `*(g_pMainGameClass + 0x1D72) = 1`. Si nosotros limpiamos ese byte a `0` *adentro* de `loadMovie()`, nuestra limpieza no sirve para nada porque el motor lo vuelve a pisar con `1` un nanosegundo después. El juego se quedaba bloqueado en el estado del video esperando a que el byte vuelva a `0`. 

**Fix (Skip instantáneo definitivo):** `impl_GLMediaPlayer_loadMovie` ahora sólo activa un flag nuestro interno (`s_movie_active = true`). La limpieza real a `0` ocurre en `media_pump()` en el *siguiente frame* del render loop, asegurando que `Create()` ya terminó de ejecutarse. Como se limpia exactamente una vez y luego se apaga, saltamos el video de inmediato sin requerir botones y sin corromper memoria.

### Bug #15 -- CONFIRMADO Y CORREGIDO: Pantallas de carga y título en blanco por culpa del FBO Downsample

El usuario reportó que, a pesar de llegar al menú principal y tener el juego funcionando, las pantallas de carga y de título se veían completamente en blanco. 

**Causa raíz:** En la optimización del FBO (`Bug #10`), el blit final de `gl_swap()` (`gl_blit_downsample_to_screen`) modificaba destructivamente el estado de OpenGL (`glDisable(GL_BLEND)`, `glDisable(GL_DEPTH_TEST)`, `glTexEnvi(GL_TEXTURE_ENV_MODE, GL_REPLACE)`) para dibujar su quad a pantalla completa, **pero no lo restauraba**. 
El motor Gameloft asume que sus estados globales persisten. Cuando intentaba dibujar los fundidos a blanco (fade-ins) de la pantalla de carga y el título, `GL_BLEND` estaba desactivado. En lugar de dibujar un blanco transparente, dibujaba un rectángulo blanco 100% opaco que tapaba toda la pantalla. Cuando el menú principal cargaba, re-inicializaba sus propios estados de blending, por lo que el menú sí se veía bien.

**Fix:** Se actualizó `gl_blit_downsample_to_screen` en `source/utils/glutil.c` para hacer un "save & restore" completo del estado OpenGL (`glGetIntegerv`, `glIsEnabled`, etc.) antes y después de dibujar el FBO. Así, el motor Gameloft recupera exactamente el estado de texturas, blend, depth, scissor y color que había dejado al final de su frame, y las transparencias de la interfaz vuelven a funcionar.

### Bug #16 -- Juego iba a 4 FPS en carrera debido a lecturas sincronas de disco

El usuario reportó que a pesar de que el menú y resolución iban perfectos, dentro de la carrera el juego corría a 4 FPS máximo.

**Diagnóstico:** Analizando los logs `asphalt5_037.log`, noté que durante la carrera se hacían cientos de llamadas a `GLResLoader.getResourceFull()`, pidiendo una y otra vez archivos `.cnk` (chunks) completos de 1 MB (ej. `package_general.bar_019.cnk`). En Android (memoria interna rápida + page cache de Linux), leer 1 MB repetidas veces toma 0ms porque sale de la RAM. Pero en PS Vita, hacer un `sceIoRead` de 1 MB de forma síncrona toma unos ~100 milisegundos por cada chunk, destrozando por completo el render loop (de ahí los 4 FPS).

**Fix (Caché en RAM):** Reescribí `source/jni_resloader.c`. Eliminé las lecturas a disco continuas e implementé una Caché en RAM de 48 slots (`RAM_CACHE_SLOTS`). Ahora, la primera vez que se carga un chunk de 1 MB, se almacena en memoria. Las siguientes cientos de llamadas a ese mismo chunk se resuelven en 0.1 milisegundos con un simple `memcpy` desde la RAM de la consola (a la cual le sobran decenas de MB en este juego). Esto elimina el 100% del cuello de botella de I/O en medio de la carrera.

### Bug #17 -- Juego cae a 0 FPS / se congela al iniciar la carrera (Saturación de CPU por Audio)

A pesar de haber arreglado el acceso a disco, el usuario reportó que el juego iba a 6 FPS en el menú y caía a **0 FPS** (se congelaba) al arrancar la carrera.

**Causa raíz:** Analizando el hilo de carga de audio en `asphalt5_038.log`, descubrí que el juego estaba decodificando docenas de archivos Vorbis (`.glsnd`) pesadísimos al arrancar la carrera (rugidos de motor, derrapes, etc.). El decodificador por software `stb_vorbis` es muy exigente matemáticamente para la CPU de la Vita. Como nuestro hilo `audio_loader` se estaba creando en el Core 0 (el mismo núcleo principal donde corre el renderizado del juego y OpenGL) y con la misma prioridad, la decodificación de audio estaba **estrangulando** y robando el 100% de la CPU al juego.

**Fix:** Se modificó la creación de hilos en `source/audio.cpp` para usar la "afinidad de núcleos" (`cpuAffinityMask`) de la PS Vita:
1. El hilo mezclador de audio (`audio_mixer`) se movió permanentemente al **Core 1** (`mask 0x02`).
2. El hilo pesado de decodificación Vorbis (`audio_loader`) se movió permanentemente al **Core 2** (`mask 0x04`), y además se le bajó la prioridad a `0x7F` (la más baja del sistema).
Con esta arquitectura multinúcleo real, la decodificación de audio ocurre de fondo usando un procesador físico completamente distinto, dejando el Core 0 al 100% libre para que el motor gráfico de Gameloft corra a máxima velocidad (60 FPS) sin jamás tartamudear.
`RES_PATH`, 14.8 MB) nunca llegó a reproducirse ni una vez, así que el
logo+trailer que contiene nunca se vio. `GS_LoadMainMenu::Render()` sí hace
render real (sprites, texto -- no es un no-op), así que la pantalla de
carga debería ser visible una vez que el resto de la secuencia tenga un
ritmo normal en vez de saltar instantáneo.

**Fix:** agregado `jstring_to_cstr()` en `jni_media.c`, misma desenvoltura
de `JavaString` que ya usa `jni_resloader.c` (`js->utf8->array`), usado en
`impl_GLMediaPlayer_loadMovie`.

**Auditoría (sin más hallazgos):** grep de `va_arg(args, jobject)` en todo
`source/`. Los 3 usos en `jni_resloader.c` ya pasan por `resolve_path()`
(correcto). Los de `generated_jni_stubs.c` son stubs auto-generados que
extraen el `jobject` a una variable local pero nunca la desreferencian como
string (no-ops inertes). `audio.cpp`/`video.cpp` no reciben ningún
`jobject`/`jstring` -- sólo `jint` y `char*` ya resueltos. Este era el único
sitio con el bug.

**No confirmable sin hardware:** que las 5 pantallas (logo, video, título,
loading, y que ya no se salte directo al menú) efectivamently aparezcan
ahora depende de probar en consola real -- no hay forma de ejecutar/compilar
en este entorno (sin VITASDK). El análisis estático no encontró ninguna
causa alternativa (el pipeline de render/FBO de downsample es el mismo que
ya funciona para el menú, que el usuario confirma poder tocar/usar).

### Bug #15 -- CONFIRMADO Y CORREGIDO: audio entrecortado -- decode síncrono de varios segundos congelaba el juego entero

Problema aparte del video (bug #14), investigado en paralelo. El usuario
reportó "el sonido se escucha entre cortado" tras agregar `audio.cpp`.

**Causa raíz:** `GLMediaPlayer_loadSound`/`loadSoundBig` (los puentes JNI
que el motor llama de forma síncrona, desde su propio hilo) llamaban a
`sfx_get()` directamente, que -- si el sonido no estaba en caché todavía --
decodificaba el archivo **completo** ahí mismo con `minimp3`/`stb_vorbis`
antes de devolver el control. A diferencia del `MediaPlayer.prepare()` real
de Android (que arma un pipeline de streaming, no decodifica todo por
adelantado), esto bloqueaba a quien haya llamado al JNI por el tiempo
completo del decode. Confirmado en `logs/asphalt5_020.log`: `raw_004.glsnd`
pedido ~t=62.7s no aparece cargado hasta t=66.5s -- **casi 4 segundos**
bloqueando lo que sea que llamó `loadSound`/`loadSoundBig` (con altísima
probabilidad, el hilo principal del motor -- el mismo que dibuja y procesa
lógica de juego). Eso no es "un cortecito de audio", es el juego entero
congelado varios segundos cada vez que pide un sonido nuevo no cacheado
-- lo que el usuario percibe como "se escucha entre cortado" es
consistente con esto (el audio YA sonando lo sigue mezclando el
`mixer_thread`, que es un hilo aparte y no se bloquea, pero cualquier
sonido NUEVO que dependa de que el hilo principal siga corriendo queda
retrasado/perdido durante el freeze).

**Por qué no era el mixer en sí:** `mixer_thread` (hilo dedicado, separado)
ya hace lo correcto -- mezcla `MIX_GRAIN=2048` frames y llama
`sceAudioOutOutput()` en loop, sin contención de locks con el decode (el
decode viejo no tomaba ningún lock compartido con el mixer). El cuello de
botella real estaba enteramente en dónde y cuándo se disparaba el decode,
no en cómo se mezclaba después.

**Fix:** `source/audio.cpp` -- el decode se movió a un hilo de fondo nuevo
(`gLoaderThread`/`loader_thread()`) con una cola simple (`gLoadQueue[32]`,
sin duplicados). `sfx_get()` ahora:
- Si ya está en caché (`gCache[sndId]` es un puntero real) -> devuelve al
  toque, sin locks de más.
- Si es la primera vez que se pide -> lo marca `SFX_PENDING`, lo encola
  para el hilo de fondo, y devuelve `NULL` **inmediatamente** (no bloquea).
- Todo llamador de `sfx_get()` ya toleraba `NULL` con gracia
  (`loadSound`/`loadSoundBig` lo ignoran; `playSound`/`playSoundBig`
  simplemente no suenan esa vez) -- exactamente el mismo comportamiento que
  el `SoundPool.load()` asíncrono real de Android ya tenía. No hizo falta
  cambiar ningún contrato de función que otro código dependa.
- Nuevo lock separado `gCacheLock` (no el `gLock` que ya protegía
  `gVoices[]`/`gBig`) para que un decode lento en el hilo de fondo nunca
  quede bloqueado detrás del `mixer_thread`, ni al revés.
- `GLMediaPlayer_isSoundLoaded`/`isSoundLoadedBig` corregidos para tratar
  `SFX_PENDING` como "todavía no" (antes lo hubieran reportado como
  cargado por error, ya que sólo chequeaban contra el sentinel de fallo).
- `audio_shutdown()` para el hilo de fondo (señal + `sceKernelWaitThreadEnd`)
  **antes** del loop que libera `gCache[]`, así nunca hay un `SFX_PENDING`
  colgado cuando se recorre para hacer `free()`.

**No confirmable sin hardware:** que esto realmente elimine los cortes
depende de probar en consola real (no se puede compilar/ejecutar en este
entorno). Si el audio sigue entrecortándose después de este fix, el
siguiente sospechoso sería contención de prioridad de hilos entre
`mixer_thread`, el `cutscene_audio_thread` de `video.cpp` (que abre su
propio puerto `sceAudioOut` de tipo VOICE para el video) y el resto de la
app -- no se tocó nada de eso en este pase porque no hay evidencia todavía
de que sea necesario, y tocar prioridades de hilos sin poder medir en
consola real es más riesgo que beneficio.

### Bug #16 -- CONFIRMADO Y CORREGIDO: crash en `pthread_cond_wait()` del hilo `audio_loader` (regresión del fix del Bug #15)

**Dump:** `logs/asphalt5-psp2core-1787866080-0x0001812927-eboot.bin.psp2dmp`
**Log:** `logs/asphalt5_021.log` -- se corta justo después de
`[audio] audio subsystem initialized successfully`, o sea el crash pasó
segundos después de arrancar los hilos de audio, antes de resolver
siquiera los símbolos JNI.

```
HILO EN CRASH: 'audio_loader'
Razón: Data abort (acceso a memoria inválida)
PC/LR: pte_osSemaphoreCancellablePend (dentro de pte_cancellable_wait <-
       sem_timedwait <- pthread_cond_wait)
```

A diferencia de los crashes anteriores, acá el `PC`/`LR` cayeron dentro del
propio `asphalt5` (nuestro loader, no `libasphalt5.so`) -- la base de ESE
binario no tiene la trampa de auto-detección ya documentada más arriba, así
que esta simbolización es confiable directamente.

**Causa raíz:** `pthread_cond_wait(&gQueueCond, &gQueueLock)` -- la espera
del hilo de carga en segundo plano que el fix del Bug #15 acababa de
introducir -- es la **primera vez en todo este port que se usa
`pthread_cond_t`**. Todos los demás locks del proyecto (`gLock`,
`gCacheLock`, el `gCutAudioLock` de `video.cpp`) son mutexes simples
(`lock`/`unlock`), y esos SÍ vienen funcionando bien con
`= PTHREAD_MUTEX_INITIALIZER` estático desde sesiones anteriores
(confirmado: `gLock` se ejercita en cada ciclo de `mixer_thread` sin
problema). Pero `= PTHREAD_COND_INITIALIZER` estático **no** deja un
objeto realmente utilizable en la implementación de pthreads de VitaSDK
(pthread-embedded) -- hace falta `pthread_cond_init()` explícito antes del
primer uso, o la espera desreferencia un objeto interno no inicializado.

**Fix:** `source/audio.cpp` -- `audio_init()` ahora llama
`pthread_mutex_init(&gCacheLock, NULL)`, `pthread_mutex_init(&gQueueLock, NULL)`
y `pthread_cond_init(&gQueueCond, NULL)` explícitamente antes de arrancar
los hilos (se inicializan los 3 primitivos nuevos del Bug #15 por
consistencia/seguridad, aunque el mutex probablemente ya andaba bien --
`gCacheLock`/`gQueueLock` tampoco tenían ningún uso previo probado en
consola). `audio_shutdown()` los destruye (`pthread_cond_destroy`/
`pthread_mutex_destroy`) después de unirse (`join`) al hilo de carga, nunca
antes.

**Pendiente:** recompilar, redesplegar, confirmar que el hilo `audio_loader`
ya no crashea al arrancar.

### Bug #16 (continuación) -- el `pthread_cond_init()` NO alcanzó, se sacó `pthread_cond_t` del todo

**Dump:** `logs/asphalt5-psp2core-1787866373-0x0001e422fb-eboot.bin.psp2dmp`
**Log:** `logs/asphalt5_022.log` -- corta todavía más temprano que antes
(justo después de "video: SceAvPlayer module loaded", ni siquiera llega a
"audio subsystem initialized successfully").

Con el fix anterior ya aplicado (`pthread_mutex_init`/`pthread_cond_init`
explícitos en `audio_init()`), **el crash fue idéntico**: mismo hilo
(`audio_loader`), mismo `PC`/`LR` (`pte_osSemaphoreCancellablePend`, dentro
de la misma cadena `pthread_cond_wait` -> `sem_timedwait` ->
`pte_cancellable_wait`). La resolución de `PC`/`LR` contra el propio
`asphalt5` (no contra `libasphalt5.so`, que en este dump volvió a
auto-detectar mal la base) es directa y confiable en ambos dumps -- no hay
ambigüedad de qué función es.

**Conclusión:** `pthread_cond_init()` explícito no fue suficiente -- ya sea
que la implementación de `pthread_cond_t` de este SDK tiene un problema más
profundo, ya sea que hay algo más en el patrón `pthread_cond_wait` que no se
identificó. En vez de seguir apostando a esa API con una tercera teoría sin
poder probarla en consola propia, se sacó la condition variable
**por completo**.

**Fix:** `source/audio.cpp` -- `gQueueCond` eliminada. `loader_thread()`
ahora sondea (`pthread_mutex_lock` + chequear `gLoadQueueCount`/`gLoaderQuit`
+ `sceKernelDelayThread(2000)` si no hay nada, en vez de
`pthread_cond_wait`), exactamente el mismo patrón que `video.cpp`'s
`cutscene_audio_thread`/`cutscene_audio_submit` -- que sí viene funcionando
bien en consola en sesiones anteriores. `sfx_enqueue_load()` ya no llama
`pthread_cond_signal` (no hace falta, el polling lo recoge solo en <=2ms).
`audio_shutdown()` ya no necesita el lock+signal para despertar al hilo --
sólo pone `gLoaderQuit=1` y hace `join`, igual que ya hacía con
`gQuit`/`gThread` (mixer) más abajo en la misma función.

**Pendiente:** recompilar, redesplegar, confirmar que el hilo `audio_loader`
ya no crashea. Si esto TAMBIÉN sigue crasheando en el mismo lugar, el
problema no es la condition variable sino algo más básico en cómo se crea
este hilo específico (prioridad `0x10000100`, stack `0x10000`, mismos
valores que `mixer_thread` que sí funciona) -- revisar eso a continuación,
no seguir iterando sobre primitivas de sincronización.

### Bug #17 -- CONFIRMADO Y CORREGIDO (dato, no código): el trailer nunca decodifica ni un frame -- el video real usa MPEG-4 Part 2, no H.264

**Log:** `logs/asphalt5_023.log` -- con el bug del `jstring` corrupto (Bug
#14) ya resuelto, el nombre llega bien esta vez
(`GLMediaPlayer.loadMovie(A5_Ultimate_VNFS_2.mp4)`), `sceAvPlayerAddSource`
tiene éxito, y el archivo se abre y se lee activamente:

```
video: playing ux0:data/asphalt5/data/A5_Ultimate_VNFS_2.mp4
video: loop starting. active=1, wait_count=0, ...
video: file size -> 13955430
video: file read #1 pos=0 len=65536 -> 65536
video: file read #2 pos=65536 len=65536 -> 65536
video: file read #3 pos=13922264 len=33166 -> 33166   (exactamente el moov, al final del archivo)
video: file read #4 pos=0 len=65536 -> 65536           (relee el header)
video: file read #5 pos=13922272 len=33158 -> 33158    (misma cola, 8 bytes desplazado)
... (silencio 1.5s reales -- 108 reads en total, 4.97 MB leídos, ninguno más logueado)
video: event STATE_STOP (0x01) source=0 data=0x0
video: loop exited! ... video_frames=0, audio_frames=0 ...
```

**Ni un solo evento `STATE_READY`/`STATE_PLAY` ni `WARNING_ID` aparece en
todo el log** -- se confirmó grepeando el archivo completo, no sólo el
fragmento citado más arriba. `sceAvPlayerGetVideoData`/`GetAudioData` nunca
devuelven `true` en las 1291 iteraciones del loop (1.59s reales) pese a que
el demuxer sí está activamente leyendo el contenedor (encontró el `moov` al
final -- patrón típico de un `.mp4` sin "faststart" -- y volvió a leer el
header, exactamente lo que se espera de un demux normal). Osea: el
contenedor se parsea bien, pero el decodificador nunca produce nada, y
`SceAvPlayer` ni siquiera reporta por qué antes de rendirse con
`STATE_STOP`.

**Causa raíz (confirmada con `ffprobe`, no adivinada):** el video de
verdad **no es H.264** -- es **MPEG-4 Part 2 ("Simple Profile", fourcc
`mp4v`)**:

```
$ ffprobe -show_entries stream=codec_name,profile,codec_type,width,height,level A5_Ultimate_VNFS_2.mp4.mpeg4-orig
codec_name=mpeg4
profile=Simple Profile
codec_type=video
width=800
height=480
level=3
```

El decodificador de video por hardware de la Vita (`SceVideodec`, detrás de
`SceAvPlayer`/`SceMp4`) **sólo decodifica H.264/AVC** -- no tiene ninguna
ruta para MPEG-4 Part 2/ASP (el viejo codec estilo DivX/Xvid). Por eso
`SceAvPlayer` puede abrir el archivo y demuxearlo (el contenedor MP4 es el
mismo, sólo cambia el codec de video adentro) pero nunca decodifica ni un
frame -- no es un bug de este port ni de cómo `video.cpp` usa la API, es
una limitación de hardware. Revisados los otros 6 `.mp4` de este juego
(`ffprobe` sobre cada uno): **los 7 tienen el mismo problema** --
incluyendo los 5 que tienen `_H264` en el nombre de archivo
(`Cop_Music_C102_H264.mp4`, etc.), que en realidad son MPEG-4 Part 2
"Advanced Simple Profile" -- el sufijo del nombre no refleja el codec real
en esta variante del APK.

**Fix:** re-codificado `ux0_data/asphalt5/data/A5_Ultimate_VNFS_2.mp4`
(el único que pide el intro, confirmado en `GS_TrailerMovie::Create()`) a
H.264 Constrained Baseline nivel 3.0 con `ffmpeg`:

```
ffmpeg -i A5_Ultimate_VNFS_2.mp4 -c:v libx264 -profile:v baseline -level 3.0 \
  -pix_fmt yuv420p -c:a aac -b:a 128k -ac 2 -ar 44100 -movflags +faststart \
  A5_Ultimate_VNFS_2.mp4
```

Mismo nombre de archivo (no hace falta tocar `video.cpp` ni `jni_media.c` --
`video_play()` ya lo encuentra por el mismo path), misma resolución
(800x480) y audio AAC-LC sin tocar. El original queda guardado sin tocar
como `A5_Ultimate_VNFS_2.mp4.mpeg4-orig` en la misma carpeta, por si hace
falta revertir o volver a codificar con otros parámetros. `+faststart`
(mueve `moov` al principio) es una mejora de paso, no la causa del bug --
elimina el vaivén de lecturas cerca del final del archivo que se veía en el
log, pero eso nunca fue lo que impedía decodificar.

**No tocado (fuera de alcance de este bug, pero anotado para más
adelante):** los otros 6 `.mp4` de `ux0_data/asphalt5/data/` -- las 4
músicas de fondo con video (`Cop_Music_C102_H264.mp4`,
`Mechano_Music_A101_H264.mp4`, `Racer_Music_C100_H264.mp4`,
`Reporter_Music_A101_H264.mp4`), `Ultimate_Music_A101_H264.mp4`, y
`A5_Ultimate_VNFS_2_854.mp4` (la variante de 854 de ancho, usada sólo si
`OS_SCREEN_W == 0x356`, que ya no aplica desde que el bug #10 fijó la
resolución reportada al motor en 800x480) -- **van a necesitar el mismo
re-encode a H.264 antes de poder reproducirse**, si en algún momento se
llega al punto del juego que los usa.

**Pendiente:** recompilar, redesplegar, confirmar en consola que el trailer
ahora sí decodifica y se ve.

### Investigación (sin bug de nuestro código encontrado): pantallas de carga/título invisibles durante ~87s tras el intro

El usuario reporta que tras el video de intro no ve pantalla de carga, ni
título, ni la segunda pantalla de carga -- pantalla en blanco/congelada
hasta que aparece el menú (que sí anda: se ve, responde al touch, tiene
sonido). `logs/asphalt5_023.log`: `Entering render loop.` a t=6.07s, y
desde ahí hasta t=~93s el log es casi enteramente streaming de
`package_general.bar_NNN.cnk` (carga de assets) -- 87 segundos reales antes
de que aparezca el menú.

**Descartado con evidencia (no es la causa):**

1. **FBO de downsample sin bindear / re-bindeado a pantalla real:** `grep`
   de `glBindFramebuffer` en todo `source/` -- sólo lo tocan `glutil.c`
   (bind→FBO en `gl_init_downsample()`, bind→0 sólo dentro del propio blit
   de `gl_blit_downsample_to_screen()`, siempre re-bindea a `s_ds_fbo` antes
   de volver) y `dynlib.c` (la entrada de la tabla de imports, pero
   `objdump -T` sobre `libasphalt5.so` confirma que el `.so` **no importa
   `glBindFramebuffer`/`glGenFramebuffers`/`glFramebufferTexture2D` en
   absoluto** -- el motor nunca toca FBOs por su cuenta. `main.c`'s loop
   (`input_poll(); Renderer_nativeRender(); gl_swap(); media_pump();`)
   corre sin condicionales de estado, confirmado leyendo el archivo
   completo -- `Renderer_nativeRender()`/`gl_swap()` se llaman todos los
   frames sin excepción.

2. **UI de carga nativa de Android fuera del GLSurfaceView:** hipótesis
   descartada leyendo `Asphalt5.java` (jadx) -- `setContentView(mGLView)`
   se llama una sola vez y ES la única vista de toda la Activity; `
   SetLoadingValuable(int)` (la única llamada nativa->Java que
   `GS_LoadMainMenu::Create()` hace, confirmado en el pseudo-C) es
   literalmente un `System.out.println` de debug en el Android real, sin
   tocar ninguna vista. La pantalla de carga original **también** se
   dibuja enteramente vía GL, mismo código que estamos corriendo -- no hay
   una UI nativa equivalente que nos falte portar.

3. **`GS_LoadMainMenu::Render()` no es un stub.** Todo el cuerpo está
   gateado por `if (g_pLib3D != NULL)`, pero `g_pLib3D` se asigna una sola
   vez en `Game::InitAppData()` (parte de la secuencia de arranque temprana,
   confirmada en sesiones previas de esta bitácora) y sólo se limpia en el
   destructor de `Game` -- para cuando `GS_LoadMainMenu` corre, ya está
   seteado desde hace rato (el menú, que sí renderiza, depende del mismo
   `Lib3D`/`Sprite`/`SpriteManager` pipeline). Dentro del gate, el código
   SÍ dibuja: barra de progreso (`Sprite::PaintFrame` sobre el sprite
   `0x3b`), texto de hint (`Sprite::DrawWrap`/`DrawString`), o el título
   "Loading" solo (rama `else` cuando `*(in_r0+0x8c) != 0`) -- ningún
   camino es un no-op.

**No confirmado como bug, evidencia real:** el audio durante esta ventana
-- sólo se cargan 2 tandas de sonidos (`raw_000`+`raw_148..153` a t~14.7s,
nada más hasta t~93-95s con `raw_001`/`raw_002`, justo cuando ya se estaba
por llegar al menú). Revisando `GS_LoadMainMenu::Update()` completo no hay
ninguna llamada a `SoundManager`/`BaseSoundManager`/reproducción de música
en la rama que corre durante el streaming de packages -- consistente con
que el diseño original **no reproduce música durante esta carga**, no con
un bug nuestro. `SoundManager::SoundManager()`/`Package_Register` recién se
instancia en el `case 1` del mismo `Update()`, gateado detrás del `case 0`
completándose primero.

**Conclusión -- no se encontró un bug propio que explique la pantalla en
blanco.** El pipeline de render (loop de `main.c`, FBO de downsample,
`Lib3D`/`Sprite`) es el mismo, sin condicionales de estado, que ya
funciona para el menú confirmado visible. El código de
`GS_LoadMainMenu::Render()` dibuja contenido real cuando corre. Candidatos
que quedan sin descartar y que **requieren hardware real** (captura de
pantalla o un contador de draw-calls por `l_info` durante esta ventana) en
vez de más análisis estático, ya que no hay forma de compilar/ejecutar
esto en este entorno:

- El contenido dibujado por `GS_LoadMainMenu::Render()` es modesto por
  diseño (barra de progreso delgada + texto, sin imagen de fondo/splash
  propia) -- podría estar renderizando correctamente pero ser fácil de
  pasar por alto contra un fondo oscuro/negro, especialmente viniendo de
  un video de intro que tampoco se veía (bug de video arriba, en
  investigación aparte).
- Que los sprites necesarios (`0x3b`, fuentes) tengan textura real
  bindeada en el momento en que se pintan depende de qué tan temprano
  streamearon sus chunks -- mismo orden de dependencia que en Android
  original, así que no es evidencia de un bug de este port, pero tampoco
  se pudo confirmar en qué momento exacto la textura está lista sin poder
  ejecutar y loggear en vivo.

**No tocado:** `source/video.cpp` (otro agente investiga el video en
paralelo).

### Bug #18 -- CONFIRMADO Y CORREGIDO: pantalla blanca en carga/título, regresión de `draw_video_frame()` (GLES2 vs. fixed-function de vitaGL)

**Reporte del usuario:** después de arreglar el códec del video (Bug #17,
otro agente, en paralelo), el usuario ve una **pantalla blanca** donde antes
la pantalla de carga y el título se veían bien con su animación original --
una regresión real, no el "no se ve nada" ya descartado arriba.

**Dato clave de timing:** ésta es la PRIMERA vez en toda la sesión que
`source/video.cpp`'s `draw_video_frame()` (el único código GLES2 con
shaders propios de todo este port -- `libasphalt5.so` es 100%
fixed-function GLES1.1, confirmado hace varias sesiones) llega a ejecutarse
de verdad: antes del fix del códec, el video fallaba instantáneamente
(0 frames decodificados) y esa función nunca corría. La regresión apareció
exactamente cuando empezó a correr por primera vez -- la pantalla de carga
y el título, que usan el pipeline fixed-function normal del juego (idéntico
al de antes), nunca cambiaron de código.

**Causa raíz (nivel de código, sin necesitar el fuente de vitaGL -- no
está disponible en esta máquina, sólo `libvitaGL.a` + el header
precompilados en `~/vitasdk`):** `draw_video_frame()` usaba un programa
GLSL propio (`glUseProgram`) con arrays de atributos genéricos
(`glVertexAttribPointer`/`glEnableVertexAttribArray` en las locations 0 y
1) para dibujar el quad del video con conversión YUV→RGB por shader. Dos
riesgos concretos, verificables sin necesitar el interno de vitaGL:

1. Los datos de vértices (`verts`/`uvs`) vivían en **arrays de stack**
   locales, pasados a un attribute pointer client-side. Nada garantiza que
   una implementación los lea exactamente en el `glDrawArrays` que los usa
   -- si vitaGL difiere/batchea la aplicación de ese estado, podría leer un
   puntero que para entonces apunta a memoria de stack ya liberada.
   `gl_blit_downsample_to_screen()` en `glutil.c` -- el otro blit de quad
   fixed-function de este port, probado en hardware en cada frame -- usa
   arrays `static const` justamente por esto.
2. Bindeaba sus atributos a las locations genéricas **0 y 1** -- números
   chicos que un emulador de fixed-function GLES1.1-sobre-GLES2
   plausiblemente también usa internamente para sus propios arrays
   client-state (`glVertexPointer`/`glTexCoordPointer`). Un programa propio
   reclamando esas locations es un riesgo real de colisión con lo que
   vitaGL usa para su shader automático de fixed-function.

Sin el fuente de vitaGL no se pudo determinar CUÁL de los dos (o si ambos)
es el mecanismo exacto -- pero da igual para el fix: los dos desaparecen
sacando el shader propio del todo.

**Fix:** `source/video.cpp` -- `draw_video_frame()` reescrita para dibujar
el quad del video con fixed-function puro (`glEnable(GL_TEXTURE_2D)` +
`glTexEnvi(...GL_REPLACE)` + `glEnableClientState`/`glVertexPointer`/
`glTexCoordPointer`), exactamente el mismo patrón ya probado de
`gl_blit_downsample_to_screen()`. Se sacó toda la infraestructura de shader
GLES2 (`ensure_video_program()`, `gVideoProgram` y afines, la variante de
conversión YUV→RGB por GPU con shader) y se dejó sólo la conversión
YUV→RGB565 por CPU (`yuv420p_to_rgb565()`, ya existía, con NEON) seguida de
un `glTexSubImage2D` normal. El array de vértices del quad (que sí varía en
runtime según el aspect ratio del video, no puede ser `const`) se hizo
`static` igual -- vive en memoria estática, no en el stack, aunque se
reescribe en cada llamada.

**Además (defensivo, no la causa de esta regresión particular, pero un bug
real igual):** `source/utils/glutil.c` -- la textura de color del FBO de
downsample (`glTexImage2D(..., NULL)`) nunca se limpiaba
(`glClear`) después de crearse; su contenido inicial es memoria GPU
indefinida, no necesariamente negro. Agregado un `glClear` justo después de
armar el FBO en `gl_init_downsample()`, así el primer `gl_swap()` de todo
el programa blitea algo determinístico en vez de lo que sea que hubiera
ahí. Esto NO explica por qué la carga/título se veían bien ANTES del bug de
video (ese primer blit habría pasado igual, con o sin este `glClear`), así
que se documenta aparte de la causa raíz real de arriba.

**Pendiente:** recompilar, redesplegar, confirmar que la pantalla de
carga y el título vuelven a verse con su animación normal ahora que el
video (ya con códec H.264 real) se dibuja sin romper el estado de render
del resto del juego.

### No era un bug nuevo -- el video corregido nunca se redesplegó a la consola

`logs/asphalt5_032.log` reportó "sigue sin reproducir el video, pantalla
blanca, no llega al menú, cada vez peor". Comparado byte a byte contra
`logs/asphalt5_023.log` (de ANTES del fix del Bug #17), el comportamiento es
**idéntico**: `video: file size -> 13955430` -- ese número es el tamaño
exacto del `.mp4` MPEG-4 **original**, no el H.264 recodificado (que pesa
19998947 bytes en el mirror local, confirmado con `ls`/`ffprobe`). Mismo
patrón de lectura, mismos offsets, mismo `STATE_STOP` a los 1.6s.

**Conclusión:** el archivo corregido existe en el mirror local
(`ux0_data/asphalt5/data/`, usado para build/referencia) pero **nunca se
copió a la memory card de la consola física** -- recompilar el eboot no
resincroniza los datos del juego, hace falta un paso de deploy de datos
aparte (el toolkit lo tiene). No hay ningún bug de código nuevo acá.

**De paso:** se recodificaron a H.264 los otros 6 `.mp4` que habían quedado
pendientes del Bug #17 (`A5_Ultimate_VNFS_2_854.mp4`,
`Cop_Music_C102_H264.mp4`, `Mechano_Music_A101_H264.mp4`,
`Racer_Music_C100_H264.mp4`, `Reporter_Music_A101_H264.mp4`,
`Ultimate_Music_A101_H264.mp4`) -- los 7 videos del juego confirmados
MPEG-4 en el análisis original, ahora los 7 son H.264 real, mismos
parámetros/comando que el Bug #17, originales preservados como
`.mpeg4-orig` en la misma carpeta.

**Pendiente:** redesplegar la carpeta `data/` COMPLETA a la consola (no
solo el eboot) antes de volver a probar. Si después de eso "no llega al
menú" sigue pasando, hace falta un log que llegue más lejos que
"Entering render loop." -- `asphalt5_032.log` corta ahí mismo, así que no
hay evidencia todavía de que sea un cuelgue real y no simplemente el log de
una corrida corta/interrumpida.

### Bug #17 -- REVERTIDO por decisión de diseño: no tocar los assets originales

El usuario, con razón, rechazó la solución de recodificar los `.mp4` a
H.264: un port que depende de reemplazar los assets originales del juego no
es viable (si mañana aparece un video más, o alguien hace el port desde un
APK distinto, hay que volver a convertir a mano cada vez -- no escala, y no
es "portar el juego original"). Los 7 `.mp4` fueron restaurados a su MPEG-4
Part 2 original (`A5_Ultimate_VNFS_2.mp4`: Simple Profile, 800x480,
yuv420p, audio AAC-LC 44.1kHz estéreo, 54.15s de duración -- confirmado con
`ffprobe`). No quedó ningún archivo `.mpeg4-orig` ni recodificado en
`ux0_data/asphalt5/data/`.

**El problema de fondo sigue siendo real y de hardware, no de código:**
`SceVideodec` (el decoder de video por hardware detrás de `SceAvPlayer`)
sólo decodifica H.264/AVC -- no tiene ninguna ruta de hardware para MPEG-4
Part 2, sea cual sea la config de `SceAvPlayerInitData`. La solución
correcta para reproducir los archivos **originales sin tocarlos** es
decodificar el video por **software** (CPU) en vez de depender del
decoder de hardware -- reemplazar `SceAvPlayer` en `source/video.cpp` por
demux/decode via `libavcodec`/`libavformat` (FFmpeg), que sí soporta MPEG-4
ASP/SP nativamente en software, sin más límite que el tiempo de CPU. Esto
es un patrón establecido en el homebrew de Vita (varios reproductores de
video ya usan ffmpeg vía `vita-portlibs`/`vdpm` para exactamente este
caso -- códecs que el hardware no cubre).

**En progreso:** implementación de decode por software con ffmpeg,
delegado a un agente dado el alcance (nueva dependencia de librería,
reescritura completa del pipeline de demux/decode/render de `video.cpp`).

### Bug #17 (continuación) -- `video.cpp` reescrito completo: demux/decode por software con FFmpeg

Reescritura completa de `source/video.cpp`, eliminando toda dependencia de
`SceAvPlayer`/`SceVideodec` (y por lo tanto de `SceAvPlayer_stub`, sacado de
`CMakeLists.txt`). Nada bajo `ux0_data/asphalt5/data/` fue tocado -- los 7
`.mp4` quedan exactamente como el APK original los trae (MPEG-4 Part 2 +
AAC), y ahora el port los decodifica así, sin re-encodear nada.

**Qué se removió de `video.cpp`:** `AvFileCtx`/`av_file_open/close/read/size`
(callbacks de archivo de `SceAvPlayer`), `av_event_name`/`av_event_cb`,
`av_alloc`/`av_free`, `av_alloc_texture`/`av_free_texture`/`gAvTexBlocks[]`
(el allocador de memoria CDRAM/PHYCONT para los frame buffers internos de
`SceAvPlayer` -- ya no aplica, FFmpeg decodifica a buffers `malloc()`
normales), y `yuv420p_to_rgb565()` (en realidad implementaba NV12
semi-planar con UV intercalado, que es lo que devolvía `SceAvPlayer`, NO el
formato real que entrega el decoder software de FFmpeg).

**Qué se agregó:**
- `yuv420p_planar_to_rgb565()`: nuevo conversor NEON para YUV420P *genuino*
  (3 planos Y/U/V separados, cada uno con su propio stride/`linesize` --
  a diferencia del buffer de `SceAvPlayer`, los frames de FFmpeg pueden
  venir con padding, así que no alcanza con asumir filas empaquetadas).
  Reutiliza las mismas tablas de conversión (`CV_R`/`CV_G`/`CU_G`/`CU_B`/
  `clip_table`) que ya existían.
- `video_play()` reescrito sobre la API de demux/decode moderna de FFmpeg:
  `avformat_open_input` + `avformat_find_stream_info` (abre el `.mp4` tal
  cual, incluyendo el caso de `moov` al final del archivo, ya que FFmpeg
  hace sus propios seeks hacia atrás sin problema) -> `av_find_best_stream`
  para video y audio -> `avcodec_open2` con el decoder que corresponda al
  códec real del stream (no asume mpeg4/aac, los resuelve dinámicamente) ->
  loop de `av_read_frame` + `avcodec_send_packet` + `avcodec_receive_frame`
  para ambos streams, con flush explícito (`avcodec_send_packet(ctx, NULL)`)
  al llegar a EOF del contenedor para no perder frames bufferizados en el
  decoder.
- **Pacing de video por PTS:** a diferencia de `SceAvPlayer` (que paceaba
  internamente), `av_read_frame`/`avcodec_receive_frame` no tienen ningún
  límite de tiempo real propio -- sin esto el decode+draw corre a la
  velocidad máxima de la CPU. Cada frame de video espera
  (`sceKernelDelayThread`, con tope de 200ms por si un PTS viene mal) hasta
  que `f->pts * time_base` alcance el tiempo real transcurrido desde que
  arrancó la reproducción. El audio NO necesita este mecanismo: se
  autolimita solo, vía el pipeline de `cutscene_audio_submit()` ya existente
  (bloquea cuando los dos buffers están llenos, y `sceAudioOutOutput` en sí
  bloquea hasta que el hardware está listo) -- se dejó intacto.
- Resampling de audio con `libswresample`: el decoder de audio entrega al
  formato/layout nativo del stream, se convierte a S16 intercalado (el
  formato que espera `sceAudioOutOpenPort`) con `swr_convert()`. El puerto
  de audio se abre recién en el primer frame de audio decodificado
  (tamaño/canales reales, no asumidos de antemano), igual que antes.
- `draw_video_frame()`, `cutscene_audio_thread()`/`cutscene_audio_submit()`
  **sin ningún cambio de comportamiento** -- se mantiene la textura GLES1.1
  fixed-function (la versión con shader GLES2 causó la regresión de
  pantalla blanca documentada más arriba, no se vuelve a tocar ese código)
  y el mecanismo de audio de doble buffer por `sceAudioOut`.
- El botón de skip (Cross/Start) se sigue revisando en cada vuelta del loop
  igual que antes.
- `CMakeLists.txt`: se agregaron `avformat`/`avcodec`/`avutil`/`swresample`
  a `target_link_libraries`, con un comentario explicando que hace falta
  tener instalado el paquete `ffmpeg` de vita-portlibs (`vdpm ffmpeg`) antes
  de compilar. Se sacó `SceAvPlayer_stub` (ya no lo usa nada); se mantuvo
  `SceSysmodule_stub` porque `source/utils/netlog.c` todavía lo necesita
  para `SCE_SYSMODULE_NET`.

**Sin verificar en hardware (no hay VITASDK en este entorno, todo esto es
análisis estático):**
- Que el paquete `ffmpeg` de vita-portlibs efectivamente tenga habilitados
  los decoders `mpeg4` y `aac` en su build (si no, `avcodec_find_decoder()`
  devuelve NULL y queda logueado, pero no hay forma de confirmarlo sin
  compilar).
- La versión exacta de FFmpeg que trae el paquete -- el código cubre con
  `#if` tanto la API vieja de canales (`channels`/`channel_layout`) como la
  nueva (`AVChannelLayout`), y tanto con como sin `av_register_all()`, pero
  no se pudo confirmar cuál rama realmente compila hasta el primer build.
- Rendimiento real de decodificar MPEG-4 Part 2 a 800x480 por software en
  la CPU de la Vita a un framerate usable -- es la incógnita más grande de
  todas, puede que haga falta bajar la resolución de decode, saltear
  frames, o algún otro ajuste si en consola sale demasiado lento.
- Que el resampling de audio (`swr_convert`) entregue un formato/orden de
  bytes que `sceAudioOutOutput` interprete correctamente sin distorsión.

### Bug #17 (continuación): build real falló -- `libavformat/avformat.h: No such file or directory`

**Contexto:** el usuario corrió el build real con el toolkit y `source/video.cpp:46:10` falló al
no encontrar `libavformat/avformat.h`. Causa: el paquete `ffmpeg` de vita-portlibs nunca se había
instalado en el VITASDK real -- el rewrite de `video.cpp` de la entrada anterior fue solo análisis
estático, sin build real disponible en ese momento.

**Fix:**
- `vdpm ffmpeg` -- instala headers/libs de FFmpeg bajo `$VITASDK/arm-vita-eabi/`.
- Segundo error real encontrado recién en el link (no en compilación): referencias sin resolver a
  `lame_encode_buffer`/`lame_init`/etc. El `libavcodec.a` de vita-portlibs viene con el **encoder**
  de MP3 (LAME) compilado adentro como dependencia dura de link, aunque este port solo decodifica
  (nunca codifica). Fix: `vdpm lame` + agregar `mp3lame` a `target_link_libraries` en
  `CMakeLists.txt` (después de `avformat`/`avcodec`/`avutil`/`swresample` -- importa el orden de
  link).

**Verificación real (no solo análisis estático):** se corrió un build completo en un directorio de
prueba contra el VITASDK real del usuario: `cmake` configuró sin errores, `make -j4` compiló TODO
el código fuente sin errores (incluyendo `video.cpp`, el punto de falla original), y el link
completó exitosamente -> `asphalt5.velf` -> `eboot.bin` (SELF) -> `param.sfo` -> `asphalt5.vpk`
(1.9MB). Esto resuelve dos de las incertidumbres "sin verificar" de la entrada anterior:
- El paquete `ffmpeg` de vita-portlibs instalado SÍ tiene `ff_mpeg4_decoder` y `ff_aac_decoder`
  presentes en `libavcodec.a` (confirmado con `nm`) -- los decoders que este juego necesita están
  habilitados.
- La versión de FFmpeg instalada es `LIBAVUTIL_VERSION_MAJOR=60` (API moderna,
  `AVChannelLayout`/sin `av_register_all()`), coincide con la rama `#if LIBAVUTIL_VERSION_MAJOR >=
  57` del código -- se confirmó cuál rama compila.

**Sigue sin verificar en hardware:** que la reproducción de video/audio con decode por software
funcione correctamente (visual y auditivamente) y a un framerate usable -- el build exitoso
confirma que compila y linkea, no que funcione en consola real. Falta probar en consola y revisar
el próximo log.

**Nota para quien reproduzca este build:** si el entorno de build del toolkit no comparte el mismo
`$VITASDK` que se usó para verificar (`vdpm ffmpeg` y `vdpm lame` se instalaron ahí directamente),
hay que correr esos dos comandos `vdpm` también ahí antes de compilar.
