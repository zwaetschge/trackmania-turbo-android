/* Pause / resume audio output while the app is in the background (the guest's
 * audio threads keep running; the device simply stops mixing and playing). */
#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <stddef.h>

void tm_audio_pause(int paused) {
    ALCcontext *c = alcGetCurrentContext();
    ALCdevice *d = c ? alcGetContextsDevice(c) : NULL;
    if (!d) return;
    if (paused) alcDevicePauseSOFT(d); else alcDeviceResumeSOFT(d);
}
