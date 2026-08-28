#include <falso_jni/FalsoJNI_Impl.h>
#include "generated_jni_table.h"
#include "jni_resloader.h"
#include "jni_media.h"
#include "audio.h"

NameToMethodID nameToMethodId[] = {
	{ 0, "isWifiActive", METHOD_TYPE_BOOLEAN },
	{ 1, "onCreate", METHOD_TYPE_VOID },
	{ 2, "getLanguage", METHOD_TYPE_INT },
	{ 3, "onPause", METHOD_TYPE_VOID },
	{ 4, "SetLoadingValuable", METHOD_TYPE_VOID },
	{ 5, "onResume", METHOD_TYPE_VOID },
	{ 6, "onStop", METHOD_TYPE_VOID },
	{ 7, "onDestroy", METHOD_TYPE_VOID },
	{ 8, "onRestart", METHOD_TYPE_VOID },
	{ 9, "onStart", METHOD_TYPE_VOID },
	{ 10, "onTrackballEvent", METHOD_TYPE_BOOLEAN },
	{ 11, "onKeyDown", METHOD_TYPE_BOOLEAN },
	{ 12, "onKeyUp", METHOD_TYPE_BOOLEAN },
	{ 13, "onTouchEvent", METHOD_TYPE_BOOLEAN },
	{ 14, "loadMovie", METHOD_TYPE_VOID },
	{ 15, "onSensorChanged", METHOD_TYPE_VOID },
	{ 16, "onAccuracyChanged", METHOD_TYPE_VOID },
	{ 17, "sendAppToBackground", METHOD_TYPE_VOID },
	{ 18, "Exit", METHOD_TYPE_VOID },
	{ 19, "LaunchBilling", METHOD_TYPE_VOID },
	{ 20, "ReleaseBillingContext", METHOD_TYPE_VOID },
	{ 21, "IsDemo", METHOD_TYPE_INT },
	{ 22, "IsDoubleOption", METHOD_TYPE_INT },
	{ 23, "GetDoubleOptionText1", METHOD_TYPE_OBJECT },
	{ 24, "GetDoubleOptionText2", METHOD_TYPE_OBJECT },
	{ 25, "GetDoubleOptionText3", METHOD_TYPE_OBJECT },
	{ 26, "onActivityResult", METHOD_TYPE_VOID },
	{ 27, "launchGetGames", METHOD_TYPE_VOID },
	{ 28, "OpenGLive", METHOD_TYPE_VOID },
	{ 29, "NotifyTrophy", METHOD_TYPE_VOID },
	{ 30, "readFromFile", METHOD_TYPE_VOID },
	{ 31, "initialize", METHOD_TYPE_VOID },
	{ 32, "Asphalt5GLSurfaceView", METHOD_TYPE_OBJECT },
	{ 33, "onWindowFocusChanged", METHOD_TYPE_VOID },
	{ 34, "onSizeChanged", METHOD_TYPE_VOID },
	{ 35, "onResume", METHOD_TYPE_VOID },
	{ 36, "onPause", METHOD_TYPE_VOID },
	{ 37, "Asphalt5Renderer", METHOD_TYPE_OBJECT },
	{ 38, "onSurfaceCreated", METHOD_TYPE_VOID },
	{ 39, "onSurfaceChanged", METHOD_TYPE_VOID },
	{ 40, "surfaceDestroyed", METHOD_TYPE_VOID },
	{ 41, "onDrawFrame", METHOD_TYPE_VOID },
	{ 42, "init", METHOD_TYPE_VOID },
	{ 43, "init", METHOD_TYPE_VOID },
	{ 44, "isSoundLoaded", METHOD_TYPE_INT },
	{ 45, "isSoundLoadedBig", METHOD_TYPE_INT },
	{ 46, "loadSound", METHOD_TYPE_VOID },
	{ 47, "loadSound", METHOD_TYPE_VOID },
	{ 48, "loadSoundBig", METHOD_TYPE_VOID },
	{ 49, "loadSoundBig", METHOD_TYPE_VOID },
	{ 50, "playSound", METHOD_TYPE_VOID },
	{ 51, "playSoundBig", METHOD_TYPE_VOID },
	{ 52, "setLoopBig", METHOD_TYPE_VOID },
	{ 53, "pauseSound", METHOD_TYPE_VOID },
	{ 54, "pauseSoundBig", METHOD_TYPE_VOID },
	{ 55, "resumeSound", METHOD_TYPE_VOID },
	{ 56, "resumeSoundBig", METHOD_TYPE_VOID },
	{ 57, "stopSound", METHOD_TYPE_VOID },
	{ 58, "stopSoundBig", METHOD_TYPE_VOID },
	{ 59, "unloadSound", METHOD_TYPE_VOID },
	{ 60, "unloadSoundBig", METHOD_TYPE_VOID },
	{ 61, "setVolume", METHOD_TYPE_VOID },
	{ 62, "setVolumeBig", METHOD_TYPE_VOID },
	{ 63, "resetSound", METHOD_TYPE_VOID },
	{ 64, "setPitch", METHOD_TYPE_VOID },
	{ 65, "stopAllSounds", METHOD_TYPE_VOID },
	{ 66, "stopAllPool", METHOD_TYPE_VOID },
	{ 67, "stopAllBig", METHOD_TYPE_VOID },
	{ 68, "destroySoundPool", METHOD_TYPE_VOID },
	{ 69, "loadMovie", METHOD_TYPE_VOID },
	{ 70, "ResumeMovie", METHOD_TYPE_VOID },
	{ 71, "isMediaPlaying", METHOD_TYPE_BOOLEAN },
	{ 72, "releaseSoundPool", METHOD_TYPE_VOID },
	{ 73, "initSoundPoolArray", METHOD_TYPE_VOID },
	{ 74, "init", METHOD_TYPE_VOID },
	{ 75, "getResourceLength", METHOD_TYPE_INT },
	{ 76, "getResourceFull", METHOD_TYPE_OBJECT },
	{ 77, "getResourceFull", METHOD_TYPE_OBJECT },
	{ 78, "getRawResource", METHOD_TYPE_OBJECT },
	{ 79, "getResourceBytes", METHOD_TYPE_OBJECT },
	{ 80, "getString", METHOD_TYPE_OBJECT },
	{ 81, "onCreate", METHOD_TYPE_VOID },
	{ 82, "onPause", METHOD_TYPE_VOID },
	{ 83, "onResume", METHOD_TYPE_VOID },
	{ 84, "onRestart", METHOD_TYPE_VOID },
	{ 85, "onStart", METHOD_TYPE_VOID },
	{ 86, "onStop", METHOD_TYPE_VOID },
	{ 87, "openBrowser", METHOD_TYPE_VOID },
	{ 88, "sendAppToBackground", METHOD_TYPE_VOID },
	{ 89, "Exit", METHOD_TYPE_VOID },
	{ 90, "BackToMainActivity", METHOD_TYPE_VOID },
	{ 91, "onKeyDown", METHOD_TYPE_BOOLEAN },
	{ 92, "onKeyUp", METHOD_TYPE_BOOLEAN },
	{ 93, "onTouchEvent", METHOD_TYPE_BOOLEAN },
	{ 94, "IGPGLSurfaceView", METHOD_TYPE_OBJECT },
	{ 95, "onWindowFocusChanged", METHOD_TYPE_VOID },
	{ 96, "onSizeChanged", METHOD_TYPE_VOID },
	{ 97, "IGPRenderer", METHOD_TYPE_OBJECT },
	{ 98, "onSurfaceCreated", METHOD_TYPE_VOID },
	{ 99, "onSurfaceChanged", METHOD_TYPE_VOID },
	{ 100, "onDrawFrame", METHOD_TYPE_VOID },
	{ 101, "isMotoPhone", METHOD_TYPE_INT },
};

MethodsBoolean methodsBoolean[] = {
	{ 0, stub_Asphalt5_isWifiActive_0 },
	{ 10, stub_Asphalt5_onTrackballEvent_10 },
	{ 11, stub_Asphalt5_onKeyDown_11 },
	{ 12, stub_Asphalt5_onKeyUp_12 },
	{ 13, stub_Asphalt5_onTouchEvent_13 },
	{ 71, GLMediaPlayer_isMediaPlaying },
	{ 91, stub_IGPActivity_onKeyDown_91 },
	{ 92, stub_IGPActivity_onKeyUp_92 },
	{ 93, stub_IGPActivity_onTouchEvent_93 },
};

MethodsByte methodsByte[] = {
};

MethodsChar methodsChar[] = {
};

MethodsDouble methodsDouble[] = {
};

MethodsFloat methodsFloat[] = {
};

MethodsInt methodsInt[] = {
	{ 2, stub_Asphalt5_getLanguage_2 },
	{ 21, stub_Asphalt5_IsDemo_21 },
	{ 22, stub_Asphalt5_IsDoubleOption_22 },
	{ 44, GLMediaPlayer_isSoundLoaded },
	{ 45, GLMediaPlayer_isSoundLoadedBig },
	// HAND-WRITTEN OVERRIDE -- see source/jni_resloader.h. The generated stub
	// returned 0, which made GamePackageMgr::Init() deref a NULL LZMAFile.
	{ 75, impl_GLResLoader_getResourceLength },
	{ 101, stub_IGPRenderer_isMotoPhone_101 },
};

MethodsLong methodsLong[] = {
};

MethodsObject methodsObject[] = {
	{ 23, stub_Asphalt5_GetDoubleOptionText1_23 },
	{ 24, stub_Asphalt5_GetDoubleOptionText2_24 },
	{ 25, stub_Asphalt5_GetDoubleOptionText3_25 },
	{ 32, stub_Asphalt5GLSurfaceView_Asphalt5GLSurfaceView_32 },
	{ 37, stub_Asphalt5Renderer_Asphalt5Renderer_37 },
	// HAND-WRITTEN OVERRIDE -- see source/jni_resloader.h.
	{ 76, impl_GLResLoader_getResourceFull },
	{ 77, stub_GLResLoader_getResourceFull_77 },
	{ 78, stub_GLResLoader_getRawResource_78 },
	// HAND-WRITTEN OVERRIDE -- see source/jni_resloader.h.
	{ 79, impl_GLResLoader_getResourceBytes },
	{ 80, stub_GLResLoader_getString_80 },
	{ 94, stub_IGPGLSurfaceView_IGPGLSurfaceView_94 },
	{ 97, stub_IGPRenderer_IGPRenderer_97 },
};

MethodsShort methodsShort[] = {
};

MethodsVoid methodsVoid[] = {
	{ 1, stub_Asphalt5_onCreate_1 },
	{ 3, stub_Asphalt5_onPause_3 },
	{ 4, stub_Asphalt5_SetLoadingValuable_4 },
	{ 5, stub_Asphalt5_onResume_5 },
	{ 6, stub_Asphalt5_onStop_6 },
	{ 7, stub_Asphalt5_onDestroy_7 },
	{ 8, stub_Asphalt5_onRestart_8 },
	{ 9, stub_Asphalt5_onStart_9 },
	// FalsoJNI's GetStaticMethodID() resolves by name only (see
	// FalsoJNI.c:845), ignoring `clazz` -- and "loadMovie" exists on both
	// Asphalt5 (id 14) and GLMediaPlayer (id 69), with id 14 sorted first.
	// nativeLoadMovie()'s GetStaticMethodID(mClassGLMediaPlayer, "loadMovie",
	// ...) therefore actually resolves to id 14, not 69: this is the one
	// that's really called. Route it to the same handler as id 69.
	{ 14, impl_GLMediaPlayer_loadMovie },
	{ 15, stub_Asphalt5_onSensorChanged_15 },
	{ 16, stub_Asphalt5_onAccuracyChanged_16 },
	{ 17, stub_Asphalt5_sendAppToBackground_17 },
	{ 18, stub_Asphalt5_Exit_18 },
	{ 19, stub_Asphalt5_LaunchBilling_19 },
	{ 20, stub_Asphalt5_ReleaseBillingContext_20 },
	{ 26, stub_Asphalt5_onActivityResult_26 },
	{ 27, stub_Asphalt5_launchGetGames_27 },
	{ 28, stub_Asphalt5_OpenGLive_28 },
	{ 29, stub_Asphalt5_NotifyTrophy_29 },
	{ 30, stub_Asphalt5_readFromFile_30 },
	{ 31, stub_Asphalt5_initialize_Trophy_31 },
	{ 33, stub_Asphalt5GLSurfaceView_onWindowFocusChanged_33 },
	{ 34, stub_Asphalt5GLSurfaceView_onSizeChanged_34 },
	{ 35, stub_Asphalt5GLSurfaceView_onResume_35 },
	{ 36, stub_Asphalt5GLSurfaceView_onPause_36 },
	{ 38, stub_Asphalt5Renderer_onSurfaceCreated_38 },
	{ 39, stub_Asphalt5Renderer_onSurfaceChanged_39 },
	{ 40, stub_Asphalt5Renderer_surfaceDestroyed_40 },
	{ 41, stub_Asphalt5Renderer_onDrawFrame_41 },
	{ 42, stub_GLMediaPlayer_init_42 },
	{ 43, stub_GLMediaPlayer_init_43 },
	{ 46, GLMediaPlayer_loadSound },
	{ 47, GLMediaPlayer_loadSound },
	{ 48, GLMediaPlayer_loadSoundBig },
	{ 49, GLMediaPlayer_loadSoundBig },
	{ 50, GLMediaPlayer_playSound },
	{ 51, GLMediaPlayer_playSoundBig },
	{ 52, GLMediaPlayer_setLoopBig },
	{ 53, GLMediaPlayer_pauseSound },
	{ 54, GLMediaPlayer_pauseSoundBig },
	{ 55, GLMediaPlayer_resumeSound },
	{ 56, GLMediaPlayer_resumeSoundBig },
	{ 57, GLMediaPlayer_stopSound },
	{ 58, GLMediaPlayer_stopSoundBig },
	{ 59, GLMediaPlayer_unloadSound },
	{ 60, GLMediaPlayer_unloadSoundBig },
	{ 61, GLMediaPlayer_setVolume },
	{ 62, GLMediaPlayer_setVolumeBig },
	{ 63, GLMediaPlayer_resetSound },
	{ 64, GLMediaPlayer_setPitch },
	{ 65, GLMediaPlayer_stopAllSounds },
	{ 66, GLMediaPlayer_stopAllPool },
	{ 67, GLMediaPlayer_stopAllBig },
	{ 68, GLMediaPlayer_destroySoundPool },
	{ 69, impl_GLMediaPlayer_loadMovie },
	{ 70, stub_GLMediaPlayer_ResumeMovie_70 },
	{ 72, GLMediaPlayer_destroySoundPool },
	{ 73, GLMediaPlayer_initSoundPoolArray },
	{ 74, stub_GLResLoader_init_74 },
	{ 81, stub_IGPActivity_onCreate_81 },
	{ 82, stub_IGPActivity_onPause_82 },
	{ 83, stub_IGPActivity_onResume_83 },
	{ 84, stub_IGPActivity_onRestart_84 },
	{ 85, stub_IGPActivity_onStart_85 },
	{ 86, stub_IGPActivity_onStop_86 },
	{ 87, stub_IGPActivity_openBrowser_87 },
	{ 88, stub_IGPActivity_sendAppToBackground_88 },
	{ 89, stub_IGPActivity_Exit_89 },
	{ 90, stub_IGPActivity_BackToMainActivity_90 },
	{ 95, stub_IGPGLSurfaceView_onWindowFocusChanged_95 },
	{ 96, stub_IGPGLSurfaceView_onSizeChanged_96 },
	{ 98, stub_IGPRenderer_onSurfaceCreated_98 },
	{ 99, stub_IGPRenderer_onSurfaceChanged_99 },
	{ 100, stub_IGPRenderer_onDrawFrame_100 },
};


// System-wide constant that applications sometimes request
char WINDOW_SERVICE[] = "window";
const int SDK_INT = 19; // Android 4.4 / KitKat

NameToFieldID nameToFieldId[] = {
	{ 0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT }, 
	{ 1, "SDK_INT", FIELD_TYPE_INT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
	{ 1, SDK_INT },
};
FieldsObject fieldsObject[] = {
	{ 0, WINDOW_SERVICE },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
