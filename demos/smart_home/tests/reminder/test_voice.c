/* SPDX-License-Identifier: Apache-2.0 */
#include "voice/smart_home_voice_play.h"
#include "voice/smart_home_voice_player.h"
#include "voice/smart_home_tts.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static int hold, entered, tones, speech, fail_tone;

int smart_home_voice_player_play_mem(const uint8_t *pcm, size_t bytes,
                                    uint32_t rate, smart_home_voice_abort_t *abort)
{
    (void)abort;
    assert(pcm && rate==16000);
    pthread_mutex_lock(&mutex);
    if (bytes==160000) {
        int nonzero=0;
        for (size_t i=0;i<6400;++i) nonzero |= pcm[i];
        assert(nonzero);
        for (size_t i=6400;i<16000;++i) assert(pcm[i]==0);
        ++tones;
    } else ++speech;
    int ret=fail_tone ? -EIO : 0;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&mutex);
    return ret;
}
#ifdef CONFIG_SMART_HOME_VOICE_TTS
void smart_home_tts_config_load(smart_home_tts_config_t *config)
{
    memset(config,0,sizeof(*config));
}
int smart_home_tts_synth_ex(const char *text, uint8_t **pcm, size_t *bytes,
                          uint32_t *rate, smart_home_voice_result_t *result)
{
    (void)text; (void)result;
    pthread_mutex_lock(&mutex);
    entered=1; pthread_cond_broadcast(&cond);
    while (hold) pthread_cond_wait(&cond,&mutex);
    pthread_mutex_unlock(&mutex);
    *pcm=calloc(1,32); *bytes=32; *rate=16000;
    return 0;
}
#endif

static void await_tones(int expected)
{
    for (int i=0;i<300;++i) {
        pthread_mutex_lock(&mutex);
        int done=tones==expected;
        pthread_mutex_unlock(&mutex);
        if (done && voice_play_state()==VOICE_PLAY_IDLE) return;
        usleep(10000);
    }
    assert(!"tone worker stalled");
}

int main(void)
{
    (void)hold; (void)entered;
    assert(!voice_play_init());
#ifdef CONFIG_SMART_HOME_VOICE_TTS
    pthread_mutex_lock(&mutex); hold=1; pthread_mutex_unlock(&mutex);
    assert(!voice_play_text("pending network request"));
    pthread_mutex_lock(&mutex);
    while (!entered) pthread_cond_wait(&cond,&mutex);
    pthread_mutex_unlock(&mutex);
    assert(!voice_play_text("queued speech"));
    assert(!voice_play_alarm(11));
    voice_play_stop(); /* stop speech must preserve the queued alarm */
    pthread_mutex_lock(&mutex); hold=0; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&mutex);
    await_tones(1);
    assert(speech==0);
#else
    assert(!voice_play_alarm(11));
    await_tones(1);
#endif
    pthread_mutex_lock(&mutex); fail_tone=1; pthread_mutex_unlock(&mutex);
    assert(!voice_play_alarm(12));
    await_tones(2);
    assert(voice_play_alarm_error(12)==-EIO);
    voice_play_deinit();
    assert(voice_play_alarm(13)==-EAGAIN);
    assert(!voice_play_init());
    voice_play_deinit();
    puts("PASS: offline PCM tone, shared queue, speech cancellation and audio failure");
    return 0;
}
