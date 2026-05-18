/*
 * upload_exclusivity.h — one-call Rule-28 wiring for audio + storage.
 *
 *   wireUploadExclusivity<TMixer>(storage)
 *     · onUploadStart → stopAsync(all, Immediate) + suspendAudio()
 *     · onUploadEnd   → resumeAudio()
 *
 *   Mirrors what every audio+storage hub would otherwise hand-write
 *   (see CLAUDE.md / Rule 28).  The storage policy's transfer-active
 *   flag is auto-wired inside the policy itself; this helper only
 *   handles the mixer suspend/resume side.
 *
 *   `TMixer` must be the concrete `AudioMixer<TI2S, TCodec>` type the
 *   board uses — same one passed to `AudioServicePolicy<TMixer>`.
 *   `TStoragePolicy` is whatever your storage alias resolves to
 *   (`StorageService` = `StorageServicePolicy<Esp32StoragePolicy>` on
 *   ESP32 etc.) — taken as a template so this header doesn't have to
 *   include `<server/storage_service.h>`.
 *
 *   Build gate: `SFX_HAS_AUDIO`.
 */

#ifndef SFX_AUDIO_UPLOAD_EXCLUSIVITY_H
#define SFX_AUDIO_UPLOAD_EXCLUSIVITY_H

#if defined(SFX_HAS_AUDIO)

#include <serial/diag_log.h>

#include "audio_mixer.h"

template <typename TMixer, typename TStoragePolicy>
inline void wireUploadExclusivity(TStoragePolicy& storage) {
    storage.onUploadStart([]() {
        TMixer& mixer = TMixer::instance();
        if (mixer.isAnyPlaying()) {
            mixer.stopAsync(-1, AudioStopMode::Immediate);
            SFX_LOG_INFO("[Audio] stop queued — upload starting");
        }
        mixer.suspendAudio();
    });
    storage.onUploadEnd([]() {
        TMixer::instance().resumeAudio();
        SFX_LOG_INFO("[Audio] resumed — upload ended");
    });
}

#endif  // SFX_HAS_AUDIO
#endif  // SFX_AUDIO_UPLOAD_EXCLUSIVITY_H
