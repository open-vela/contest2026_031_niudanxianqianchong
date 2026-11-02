/* SPDX-License-Identifier: Apache-2.0 */
/* Real TTS request/decode/resample code; fake only the HTTP/TLS boundary. */
#include "smart_home_tts.c"
#include <assert.h>
#include <math.h>
#include "smart_home_voice_resample.h"

static int credential_result, transport_result, invalid_body, deny_converted;
static unsigned status_code=200, source_rate=24000, http_calls;
static uint64_t fake_now(void *context) { (void)context; return 1; }
void *smart_home_bulk_alloc(size_t size)
{ return deny_converted && size==32000 ? NULL : malloc(size); }
void smart_home_bulk_free(void *p) { free(p); }
int smart_home_secrets_get_api_key(smart_home_secret_service_t service,
    const char *backend, char *key, size_t size)
{
    assert(service==SMART_HOME_SECRET_TTS && !strcmp(backend,"mimo"));
    if (credential_result) return credential_result;
    snprintf(key,size,"test-tts-only");
    return 0;
}
const char *smart_home_secrets_error_text(int result)
{ return result ? "credential failure" : "configured"; }
agent_error_t agent_port_openvela_runtime_init(agent_runtime_t *runtime)
{ memset(runtime,0,sizeof(*runtime)); runtime->now_ms=fake_now; return 0; }
agent_error_t agent_port_openvela_tls_init(agent_port_openvela_tls_t *state,
    const agent_port_openvela_tls_config_t *config)
{ memset(state,0,sizeof(*state)); state->config=*config; return 0; }

static void put16(uint8_t *p,uint16_t v) { p[0]=v; p[1]=v>>8; }
static void put32(uint8_t *p,uint32_t v) { put16(p,v); put16(p+2,v>>16); }
static agent_error_t fake_request(void *ctx,const agent_http_request_t *request,
    const agent_http_sink_t *sink)
{
    (void)ctx;
    http_calls++;
    assert(request->headers[0].value.size==strlen("Bearer test-tts-only"));
    assert(!memcmp(request->headers[0].value.data,"Bearer test-tts-only",
                   request->headers[0].value.size));
    cJSON *body=cJSON_Parse(request->body);
    assert(body);
    cJSON *messages=cJSON_GetObjectItem(body,"messages");
    assert(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(messages,0),"role")->valuestring,"assistant"));
    cJSON_Delete(body);
    if (transport_result) return transport_result;
    assert(sink->headers(sink->context,status_code,NULL,0)==0);
    if (status_code!=200) return sink->body(sink->context,"{}",2);
    if (invalid_body) return sink->body(sink->context,"{bad}",5);
    size_t bytes=source_rate*2;
    uint8_t *wav=calloc(1,bytes+44);
    memcpy(wav,"RIFF",4); put32(wav+4,bytes+36); memcpy(wav+8,"WAVEfmt ",8);
    put32(wav+16,16); put16(wav+20,1); put16(wav+22,1);
    put32(wav+24,source_rate); put32(wav+28,source_rate*2);
    put16(wav+32,2); put16(wav+34,16); memcpy(wav+36,"data",4); put32(wav+40,bytes);
    for (size_t i=0;i<bytes;i+=2) put16(wav+44+i,1234);
    size_t capacity=(bytes+46)/3*4+4, encoded=0;
    unsigned char *b64=malloc(capacity);
    assert(mbedtls_base64_encode(b64,capacity,&encoded,wav,bytes+44)==0);
    free(wav);
    cJSON *root=cJSON_CreateObject(), *choices=cJSON_AddArrayToObject(root,"choices");
    cJSON *choice=cJSON_CreateObject(); assert(cJSON_AddItemToArray(choices,choice));
    cJSON *message=cJSON_AddObjectToObject(choice,"message");
    cJSON *audio=cJSON_AddObjectToObject(message,"audio");
    assert(cJSON_AddStringToObject(audio,"data",(const char *)b64)); free(b64);
    char *json=cJSON_PrintUnformatted(root); cJSON_Delete(root);
    agent_error_t result=sink->body(sink->context,json,strlen(json)); free(json);
    return result;
}
agent_error_t agent_port_openvela_transport_init(agent_transport_t *out,
    agent_port_openvela_transport_t *state,const agent_port_openvela_transport_config_t *config)
{
    static const agent_transport_ops_t ops={fake_request};
    assert(config->response_header_capacity>=16);
    assert(config->response_header_buffer_size>=2048);
    state->config=*config; out->ops=&ops; out->context=state; return 0;
}

static double rms(const uint8_t *pcm,size_t count)
{
    double total=0;
    for (size_t i=32;i<count-32;i++) {
        double x=(int16_t)((unsigned)pcm[2*i]|((unsigned)pcm[2*i+1]<<8));
        total+=x*x;
    }
    return sqrt(total/(count-64));
}
static double test_tone(unsigned hz)
{
    uint8_t input[48000], output[32000]; size_t size;
    for (size_t i=0;i<24000;i++) {
        int16_t value=(int16_t)(12000*sin(2*3.141592653589793*hz*i/24000));
        put16(input+i*2,(uint16_t)value);
    }
    assert(!smart_home_voice_resample_24k_16k(input,sizeof(input),output,sizeof(output),&size));
    assert(size==32000);
    if (hz==1000) {
        unsigned crossings=0;
        for (size_t i=1;i<16000;i++) {
            int16_t a=(int16_t)((unsigned)output[2*i-2]|((unsigned)output[2*i-1]<<8));
            int16_t b=(int16_t)((unsigned)output[2*i]|((unsigned)output[2*i+1]<<8));
            if (a<=0 && b>0) crossings++;
        }
        assert(crossings>=999 && crossings<=1001);
    }
    return rms(output,16000);
}
int main(void)
{
    uint8_t *pcm=NULL; size_t bytes=0; uint32_t rate=0;
    smart_home_voice_result_t result;
    /* Seed the real config cache; no real credentials or network are touched. */
    memset(&g_tts_config,0,sizeof(g_tts_config)); g_tts_config_loaded=1;
    strcpy(g_tts_config.backend_id,"mimo"); strcpy(g_tts_config.model,"tts");
    strcpy(g_tts_config.host,"example.invalid"); strcpy(g_tts_config.path,"/chat");
    strcpy(g_tts_config.port,"443"); strcpy(g_tts_config.voice,"mimo_default");
    credential_result=AGENT_ERROR_NOT_FOUND;
    assert(smart_home_tts_synth_ex("test",&pcm,&bytes,&rate,&result)==credential_result);
    assert(result.stage==SMART_HOME_VOICE_CREDENTIAL && !http_calls && !pcm);
    credential_result=0; transport_result=AGENT_ERROR_CAPACITY;
    assert(smart_home_tts_synth_ex("test",&pcm,&bytes,&rate,&result)==transport_result);
    assert(result.stage==SMART_HOME_VOICE_NETWORK && result.code==transport_result);
    transport_result=0;
    const unsigned errors[]={401,403,429,500};
    for (size_t i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
        status_code=errors[i];
        assert(smart_home_tts_synth_ex("test",&pcm,&bytes,&rate,&result)!=0);
        assert(result.stage==SMART_HOME_VOICE_HTTP && result.http_status==errors[i]);
    }
    status_code=200; invalid_body=1;
    assert(smart_home_tts_synth_ex("test",&pcm,&bytes,&rate,&result)!=0);
    assert(result.stage==SMART_HOME_VOICE_RESPONSE); invalid_body=0;
    deny_converted=1;
    assert(smart_home_tts_synth_ex("test",&pcm,&bytes,&rate,&result)==AGENT_ERROR_NOMEM);
    assert(result.stage==SMART_HOME_VOICE_RESOURCE && !pcm); deny_converted=0;
    assert(!smart_home_tts_synth_ex("test",&pcm,&bytes,&rate,&result));
    assert(result.code==0 && rate==16000 && bytes==32000 && pcm);
    for (size_t i=0;i<bytes;i+=2) assert((int16_t)((unsigned)pcm[i]|((unsigned)pcm[i+1]<<8))==1234);
    free(pcm);
    source_rate=16000;
    assert(!smart_home_tts_synth("test",&pcm,&bytes,&rate));
    assert(rate==16000 && bytes==32000); free(pcm);
    assert(pthread_mutex_lock(&g_tts_request_mutex)==0);
    assert(smart_home_tts_synth_ex("test",&pcm,&bytes,&rate,&result)==AGENT_ERROR_BUSY);
    assert(result.stage==SMART_HOME_VOICE_RESOURCE && !pcm);
    pthread_mutex_unlock(&g_tts_request_mutex);
    puts("PASS: actual TTS credentials, capacity, HTTP, decode, allocation, resampling and busy recovery");
    double pass=test_tone(1000), stop=test_tone(10000);
    printf("resampler: passband RMS=%.3f, 10kHz alias RMS=%.3f (%.2f dB)\n",pass,stop,20*log10(stop/pass));
    assert(pass>8300 && pass<8600 && stop/pass<0.01);
    uint8_t small[8]={0}; size_t size=99;
    assert(smart_home_voice_resample_24k_16k(small,3,small+4,4,&size)==-EINVAL && size==0);
    assert(smart_home_voice_resample_24k_16k(small,4,small+4,0,&size)==-ENOSPC);
    const int extremes[]={32767,-32768};
    for (size_t sign=0;sign<2;sign++) {
        for (size_t i=0;i<3;i++) put16(small+2*i,(uint16_t)extremes[sign]);
        uint8_t output[4];
        assert(!smart_home_voice_resample_24k_16k(small,6,output,sizeof(output),&size));
        assert(size==4);
        for (size_t i=0;i<2;i++)
            assert((int16_t)((unsigned)output[2*i]|((unsigned)output[2*i+1]<<8))==extremes[sign]);
    }
    /* Truncated fmt/data chunks must fail before reading PCM header fields. */
    uint8_t wav[44]={0}; size_t offset, length; uint32_t wav_rate;
    memcpy(wav,"RIFF",4); memcpy(wav+8,"WAVEfmt ",8); put32(wav+16,100);
    assert(tts_parse_wav(wav,sizeof(wav),&offset,&length,&wav_rate)==-EINVAL);
    put32(wav+16,16); put16(wav+20,1); put16(wav+22,1);
    put32(wav+24,24000); put16(wav+34,16); memcpy(wav+36,"data",4);
    put32(wav+40,2);
    assert(tts_parse_wav(wav,sizeof(wav),&offset,&length,&wav_rate)==-EINVAL);
    puts("PASS: duration, tone pitch, anti-alias rejection, buffer and truncated WAV bounds");
    return 0;
}
