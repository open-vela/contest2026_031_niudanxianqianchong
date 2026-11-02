/* SPDX-License-Identifier: Apache-2.0 */
/* Run the real queue/worker; fake synthesis and the audio device boundary. */
#include <pthread.h>
#include <assert.h>
#include <time.h>
/* Linux SCHED_OTHER rejects NuttX numeric priorities. All other pthread APIs
 * (including the explicitly allocated stack) remain real. */
static int host_priority(pthread_attr_t *attr,const struct sched_param *param)
{ (void)attr; (void)param; return 0; }
#define pthread_attr_setschedparam host_priority
#include "smart_home_voice_play.c"
#undef pthread_attr_setschedparam

static int synth_code, play_code, calls;
void *smart_home_bulk_alloc(size_t bytes) { return malloc(bytes); }
void smart_home_bulk_free(void *p) { free(p); }
void smart_home_tts_config_load(smart_home_tts_config_t *config)
{ memset(config,0,sizeof(*config)); config->enabled=1; strcpy(config->backend_id,"mimo"); }
int smart_home_tts_synth_ex(const char *text,uint8_t **pcm,size_t *bytes,
    uint32_t *rate,smart_home_voice_result_t *result)
{
    (void)text;
    *result=(smart_home_voice_result_t){synth_code,401,SMART_HOME_VOICE_HTTP};
    *pcm=synth_code?NULL:malloc(320); *bytes=320; *rate=16000;
    pthread_mutex_lock(&g_voice_mutex);
    calls++;
    pthread_mutex_unlock(&g_voice_mutex);
    return synth_code;
}
int smart_home_voice_player_play_mem(const uint8_t *pcm,size_t bytes,
    uint32_t rate,smart_home_voice_abort_t *abort)
{ assert(pcm && bytes==320 && rate==16000 && abort); return play_code; }
static void complete(int expected)
{
    struct timespec delay={0,1000000};
    for (int i=0;i<3000;i++) {
        pthread_mutex_lock(&g_voice_mutex);
        int done=calls==expected && !g_voice_queue && g_voice_state==VOICE_PLAY_IDLE;
        pthread_mutex_unlock(&g_voice_mutex);
        if (done) return;
        nanosleep(&delay,NULL);
    }
    assert(!"worker timed out");
}
int main(void)
{
    smart_home_voice_result_t result;
    assert(!voice_play_init());
    synth_code=AGENT_ERROR_MODEL_FAILED;
    assert(!voice_play_text("HTTP failure")); complete(1);
    assert(voice_play_take_error(&result));
    assert(result.stage==SMART_HOME_VOICE_HTTP && result.http_status==401);
    assert(!voice_play_take_error(&result));
    synth_code=0; play_code=-EINVAL;
    assert(!voice_play_text("playback failure")); complete(2);
    assert(voice_play_take_error(&result));
    assert(result.stage==SMART_HOME_VOICE_PLAYBACK && result.code==-EINVAL);
    play_code=-EINTR;
    assert(!voice_play_text("intentional cancellation")); complete(3);
    assert(!voice_play_take_error(&result));
    play_code=0;
    assert(!voice_play_text("retry")); complete(4);
    assert(!voice_play_take_error(&result));
    play_code=-EINVAL;
    assert(!voice_play_text("old lifecycle")); complete(5);
    voice_play_deinit();
    assert(!voice_play_init());
    assert(!voice_play_take_error(&result));
    voice_play_deinit();
    puts("PASS: worker forwards HTTP/playback errors once, ignores cancellation and clears stale errors on restart");
    return 0;
}
