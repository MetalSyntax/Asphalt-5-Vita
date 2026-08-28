# Plan de Port — Asphalt 5 (PS Vita)

> Generado por psvita-port-toolkit el 2026-08-23. Punto de partida con lo detectado automáticamente --
confirmar todo con objdump/Ghidra/jadx a mano antes de asumirlo como cierto.

## 0. Contexto

- **Juego:** Asphalt 5
- **Paquete Java:** com.gameloft.android.GAND.GloftA5HD
- **APK original:** `Asphalt-5-HD-v3.4.1.apk`
- **TITLEID asignado:** `ASPHALT05`

**¿Motor conocido?** Revisar si algún port hermano (bajo la misma BASE_DIR) comparte motor antes de
reusar su código -- confirmar con símbolos JNI reales, no por analogía superficial.

## 1. Detección automática

- **ABI(s):** armeabi
- **ABI elegida:** armeabi
- **Nota de arquitectura:** Solo armeabi (ARMv6, soft-float) -- Vita lo ejecuta igual (ARMv7 es superset), sin NEON de v7a.
- **Versión de GLES:** valor no estándar en manifest: 0x20000 (declarado en AndroidManifest.xml)

## 2. .so encontrados (ABI armeabi)

- `asphalt5_extract/lib/armeabi/._libasphalt5.so` (4 KB)
- `asphalt5_extract/lib/armeabi/._libigp.so` (4 KB)
- `asphalt5_extract/lib/armeabi/libasphalt5.so` (2479 KB)
- `asphalt5_extract/lib/armeabi/libigp.so` (331 KB)


## 3. Exports JNI (convención `Java_*`)

**Corregido a mano (2026-08-27).** La detección automática falló, pero
`libasphalt5.so` **sí** exporta por convención de nombre -- `nm -C` lista los
`Java_com_gameloft_android_GAND_GloftA5HD_*`. No hay `RegisterNatives` y
**no hay `JNI_OnLoad`**.

Puntos de entrada que usa el arranque (offsets en el .so, base `0x98000000`):

| Símbolo | Offset | Firma real |
|---|---|---|
| `Asphalt5Renderer_nativeGetJNIEnv` | `0x5d694` | `(JNIEnv*, jobject)` |
| `GLResLoader_nativeInit` | `0x5ebb4` | `(JNIEnv*, jclass, jint)` — static |
| `GLMediaPlayer_nativeInit` | `0x5dbfc` | `(JNIEnv*, jclass, jint)` — static |
| `Asphalt5_nativeInit` | `0x5cbc4` | `(JNIEnv*, jclass, jint, jint)` — static |
| `Asphalt5Renderer_nativeInit` | `0x5db58` | `(JNIEnv*, jobject, jint×5)` |
| `Asphalt5Renderer_nativeRender` | `0x5db48` | `(JNIEnv*, jclass)` — static |
| `Asphalt5Renderer_nativeResize` | `0x5d6b4` | no-op (`bx lr`) |


## 4. Checklist

- [x] Repo creado desde soloader-boilerplate, git init, .gitignore anti-DMCA.
- [x] APK decompilado (jadx) y .so decompilado(s) (Ghidra) -- ver sección 2/3.
- [x] Análisis del motor real (ciclo de vida nativo, reuso de otro port o boilerplate genérico).
- [x] Bootstrap del loader: so_file_load/so_relocate/so_resolve, primer build.
- [x] Tabla JNI (FalsoJNI): registrar exports + callbacks hacia "Java".
- [~] Primer arranque en consola real (bug #1 `mEnv==NULL` corregido; pendiente validar en hardware).
- [ ] Gráficos (wrappers GL según versión detectada).
- [ ] Input, Audio, Assets, LiveArea/VPK.
- [ ] Pruebas en hardware real.

## 5. Herramientas

Este port se gestiona con **psvita-port-toolkit** (standalone, fuera de este repo). Desde el
toolkit: `Continuar con un port existente` → elegí esta carpeta (ya tiene `.psvita-toolkit.json`).

## Auto-detected lifecycle methods (psvita-toolkit)

Native methods whose name matches a well-known Android/GL app lifecycle hook --
these are the ones `main.c`/the loader most likely needs to call directly to
drive the game (there's no real Android `Activity`/`GLSurfaceView` calling them
for you).

- `com.gameloft.android.GAND.GloftA5HD.Asphalt5.nativeInit(int, int)`
- `com.gameloft.android.GAND.GloftA5HD.Asphalt5Renderer.nativeInit(int, int, int, int, int)`
- `com.gameloft.android.GAND.GloftA5HD.Asphalt5Renderer.nativeRender(void)`
- `com.gameloft.android.GAND.GloftA5HD.Asphalt5Renderer.nativeResize(int, int)`
- `com.gameloft.android.GAND.GloftA5HD.GLMediaPlayer.nativeInit(int)`
- `com.gameloft.android.GAND.GloftA5HD.GLResLoader.nativeInit(int)`
- `com.gameloft.android.GAND.GloftA5HD.IGPActivity.nativeInit(void)`
- `com.gameloft.android.GAND.GloftA5HD.IGPRenderer.nativeInit(int, int, int)`
- `com.gameloft.android.GAND.GloftA5HD.IGPRenderer.nativeRender(void)`
- `com.gameloft.android.GAND.GloftA5HD.IGPRenderer.nativeResize(int, int)`

## Ciclo de vida CONFIRMADO (objdump + jadx, 2026-08-27)

Reemplaza la lista auto-detectada de arriba, que no tiene orden y omite
`nativeGetJNIEnv` -- el paso sin el cual todo lo demás hace data abort.

El motor **cachea su propio `JNIEnv*`** en el global `mEnv` (`.so + 0x1f84e8`) y
todos los demás entry points desreferencian ese cache, **no** el `env` que
reciben por parámetro. `mEnv` lo setea sólo `nativeGetJNIEnv`
(`mEnv = env`, vía `GOT[0x4e8]`). Sin ese primer paso:
`ldr r2,[mEnv]` → `0`, `ldr r3,[r2]` → data abort. Ver bug #1 en
`port_progress.md`.

Orden exacto, tomado de `Asphalt5Renderer.onSurfaceCreated()` (jadx):

```c
gl_init();                                             // GXM vivo primero:
                                                       // Renderer_nativeInit llama
                                                       // importGLInit() + appInit()
nativeGetJNIEnv(&jni, &jni);                           // 1. primea mEnv
GLResLoader_nativeInit(&jni, &jni, 0);                 // 2.
GLMediaPlayer_nativeInit(&jni, &jni, 0);               // 3.
Asphalt5_nativeInit(&jni, &jni, 0, /*pvrt*/1);         // 4. -> mbUsePVRT
Renderer_nativeInit(&jni, &jni, 0, 1, 960, 544, 0);    // 5. -> appInit(w,h,lang)
while (1) { Renderer_nativeRender(&jni, &jni); gl_swap(); }
```

Notas confirmadas:

- `Asphalt5_nativeInit` descarta su 1er `jint`; el 2do va a `mbUsePVRT`
  (`GOT[0x874] -> mbUsePVRT`). Java lo pasa en `1` salvo en HTC / Sony X10.
- `Asphalt5_nativeInit` resuelve 13 `jmethodID` estáticos de la clase `Asphalt5`
  (`sendAppToBackground`, `Exit`, `LaunchBilling`, `ReleaseBillingContext`,
  `IsDemo`, `IsDoubleOption`, `GetDoubleOptionText1..3`, `launchGetGames`,
  `OpenGLive`, `NotifyTrophy`, `SetLoadingValuable`). Todos ya están en
  `source/java.c`.
- `Renderer_nativeInit` tiene un flag de "ya inicializado": si se llama dos
  veces, la 2da sólo guarda un puntero y sale (no re-inicializa GL).
- `Renderer_nativeResize` es `bx lr` -- no hace nada, no hace falta llamarlo.
- `libigp.so` es el SDK de publicidad/cross-promo de Gameloft. `libasphalt5.so`
  no depende de él; se puede ignorar para el arranque.

## Pipeline de assets CONFIRMADO (2026-08-27)

`libasphalt5.so` **no abre sus archivos de datos**. Todo pasa por tres métodos
estáticos de Java que resuelve `GLResLoader_nativeInit` (`.so + 0x5ebb4`):

| Método | Firma | Devuelve |
|---|---|---|
| `getResourceLength` | `(Ljava/lang/String;)I` | tamaño, o `0` si no está |
| `getResourceFull` | `(Ljava/lang/String;)[B` | archivo completo |
| `getResourceBytes` | `(Ljava/lang/String;II)[B` | `loadSize` bytes desde `offset` |

Implementados a mano en `source/jni_resloader.c` (leen de `DATA_PATH`), no en
`generated_jni_stubs.c` -- que es auto-generado y los devolvía `0`/`NULL`.

En Android buscaban en 3 lugares; verificado que sólo uno importa acá:

1. `res/drawable/res_<name>` (recursos "protegidos") -- **no existen** en este
   APK; su `res/drawable/` sólo trae la UI de IGP/billing (73 PNGs).
2. `GLMediaPlayer.SOUND_DIR` = `/sdcard/gameloft/games/asphalt5/` -- **ésta es
   la fuente real**, mapea a `RES_PATH` en la Vita (`ux0:data/asphalt5/data/`).
3. Assets del APK -- **el APK no tiene `assets/`**.

**Data:** 327 archivos en `Asphalt-5-HD-v3.4.1-adreno.zip` bajo `asphalt5/`
(plano, sin subdirectorios), 163 MB. Incluye 93 `package_general.bar_*.cnk`
(el contenedor `package_general.bar` partido en chunks de 1 MiB), 187
`.glsnd`, 13 `.bsprite`, 7 `.mp4`, 5 `.bar`, 2 `.map` y algunos archivos sin
extensión (`demodata`, `IGPConfig`, `igpdata`, `textures`, `window`).
`missionslib.bar` **no** es un archivo: vive dentro de `package_general.bar`.

**Layout en disco (2026-08-27):** estos 327 archivos van bajo
`ux0:data/asphalt5/data/` (subcarpeta dedicada, resuelta como `RES_PATH` --
`DATA_PATH` + `data/`), **no** sueltos directamente en `ux0:data/asphalt5/`.
Ese nivel raíz (`DATA_PATH`) queda para lo que no es "resource" del
`GLResLoader`: los `.so` (`SO_PATH`), `logs/`, `saves/`, `config.txt`, el
cache de shaders (`gxp/`, `cg/`, `glsl/`) y `netlog.txt`. `jni_resloader.c`
(`resolve_path()`) prefija cada nombre que pide el motor con `RES_PATH`, no
con `DATA_PATH`. Ver `CMakeLists.txt` para la definición de `RES_PATH`.

## Trampa: `vita-parse-core` detecta mal la base del .so

Pasó en los dos crashes analizados hasta ahora, con un valor distinto cada vez
(`0x80ee2000` y `0x98050000`). Cuando se equivoca, **todos** los símbolos que
imprime están corridos y manda a leer funciones que no tienen nada que ver
(reportó `Sprite::DrawString` donde en realidad era `LZMAFile::OpenAttached`).

La base real es **`0x98000000`** (`LOAD_ADDRESS` en `source/utils/init.c`).

Dos formas de verificarlo en 5 segundos antes de creerle al reporte:

- `_GLOBAL_OFFSET_TABLE_` está en `.so + 0x1f4ed0`. Si algún registro tiene
  `0x981f4ed0` (es habitual: el código usa direccionamiento GOT-relativo), la
  base es `0x98000000`.
- El log incremental imprime las direcciones resueltas al arrancar:
  `nativeGetJNIEnv -> 0x9805d694` y `nm` dice `0x5d694`. Base = resta.

Para re-simbolizar a mano: `offset = PC - 0x98000000`, después buscar en
`arm-vita-eabi-nm -C --defined-only libasphalt5.so` ordenado.

