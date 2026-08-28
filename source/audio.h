#ifndef ASPHALT5_AUDIO_H
#define ASPHALT5_AUDIO_H

#include <falso_jni/FalsoJNI.h>

#ifdef __cplusplus
extern "C" {
#endif

void audio_init(void);
void audio_shutdown(void);

// Direct C audio API (for patch.c and internal engine hooks)
void audio_play_sound(int sndId, int instance, float vol);
void audio_play_sound_big(int sndId, float vol, int loop);
void audio_stop_all(void);

// GLMediaPlayer JNI sound bridge
jint GLMediaPlayer_isSoundLoaded(jmethodID id, va_list args);
jint GLMediaPlayer_isSoundLoadedBig(jmethodID id, va_list args);
void GLMediaPlayer_loadSound(jmethodID id, va_list args);
void GLMediaPlayer_loadSoundBig(jmethodID id, va_list args);
void GLMediaPlayer_playSound(jmethodID id, va_list args);
void GLMediaPlayer_playSoundBig(jmethodID id, va_list args);
void GLMediaPlayer_setLoopBig(jmethodID id, va_list args);
void GLMediaPlayer_pauseSound(jmethodID id, va_list args);
void GLMediaPlayer_pauseSoundBig(jmethodID id, va_list args);
void GLMediaPlayer_resumeSound(jmethodID id, va_list args);
void GLMediaPlayer_resumeSoundBig(jmethodID id, va_list args);
void GLMediaPlayer_stopSound(jmethodID id, va_list args);
void GLMediaPlayer_stopSoundBig(jmethodID id, va_list args);
void GLMediaPlayer_unloadSound(jmethodID id, va_list args);
void GLMediaPlayer_unloadSoundBig(jmethodID id, va_list args);
void GLMediaPlayer_setVolume(jmethodID id, va_list args);
void GLMediaPlayer_setVolumeBig(jmethodID id, va_list args);
void GLMediaPlayer_resetSound(jmethodID id, va_list args);
void GLMediaPlayer_setPitch(jmethodID id, va_list args);
void GLMediaPlayer_stopAllSounds(jmethodID id, va_list args);
void GLMediaPlayer_stopAllPool(jmethodID id, va_list args);
void GLMediaPlayer_stopAllBig(jmethodID id, va_list args);
void GLMediaPlayer_destroySoundPool(jmethodID id, va_list args);
void GLMediaPlayer_initSoundPoolArray(jmethodID id, va_list args);
jboolean GLMediaPlayer_isMediaPlaying(jmethodID id, va_list args);

#ifdef __cplusplus
}
#endif

#endif // ASPHALT5_AUDIO_H
