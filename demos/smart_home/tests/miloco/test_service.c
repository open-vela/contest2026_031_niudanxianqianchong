/* SPDX-License-Identifier: Apache-2.0 */
#include "../../src/miloco/smart_home_miloco_service.c"
#include <assert.h>
static const char *read_body, *post_body;
static int get_error, post_error, posts, reads;
static char sent[256];
int smart_home_miloco_http_get(const smart_home_miloco_client_config_t *cfg,
    const char *path, char *body, size_t capacity, int *status)
{
    (void)cfg;
    assert(strstr(path,"/status?iid=prop.9.6"));
    reads++;
    *status=200;
    snprintf(body,capacity,"%s",read_body);
    return get_error;
}
int smart_home_miloco_http_post(const smart_home_miloco_client_config_t *cfg,
    const char *path, const char *request, char *body, size_t capacity, int *status)
{
    (void)cfg;
    assert(strstr(path,"/devices/test/control"));
    snprintf(sent,sizeof(sent),"%s",request);
    posts++;
    *status=200;
    snprintf(body,capacity,"%s",post_body);
    return post_error;
}
int main(void)
{
    smart_home_miloco_t *s=calloc(1,sizeof(*s));
    assert(s && !pthread_mutex_init(&s->lock,NULL) && !sem_init(&s->wake,0,0));
    smart_home_miloco_config_t cfg = {.port=1810};
    strcpy(cfg.host, "gateway.local");
    strcpy(cfg.token, "example");
    assert(smart_home_miloco_save_status(s) == AGENT_ERROR_NOT_FOUND);
    assert(smart_home_miloco_request_save(s, &cfg) == AGENT_OK);
    assert(smart_home_miloco_save_status(s) == AGENT_ERROR_BUSY);
    assert(smart_home_miloco_request_save(s, &cfg) == AGENT_ERROR_BUSY);
    s->save_pending = false;
    s->save_in_progress = true;
    assert(smart_home_miloco_save_status(s) == AGENT_ERROR_BUSY);
    s->save_in_progress = false;
    s->save_completed = true;
    s->save_result = AGENT_ERROR_IO;
    assert(smart_home_miloco_save_status(s) == AGENT_ERROR_IO);
    assert(smart_home_miloco_request_save(s, &cfg) == AGENT_OK);
    s->save_pending = false;
    s->save_result = AGENT_OK;
    s->save_completed = true;
    assert(smart_home_miloco_save_status(s) == AGENT_OK);
    s->device_count=1;
    smart_home_miloco_device_t *d=&s->devices[0];
    strcpy(d->did,"test"); d->online=true; d->control_count=1;
    smart_home_miloco_control_t *c=&d->controls[0];
    strcpy(c->iid,"prop.9.6"); strcpy(c->type_name,"on");
    c->type=SMART_HOME_MILOCO_CTRL_BOOL; c->value_valid=true;
    c->value=0; c->observation=1;
    read_body="{\"code\":0,\"data\":{\"properties\":[{\"iid\":\"prop.9.6\",\"value\":true,\"code\":0}]}}";
    post_body="{\"code\":0,\"data\":{\"results\":[{\"siid\":9,\"piid\":6,\"code\":0}]}}";
    char body[1024];
    assert(!smart_home_miloco_submit_power(s,"test",true));
    assert(s->pending_count==1 && !strcmp(s->pending[0].iid,"prop.9.6"));
    assert(!execute_control(s,&s->pending[0],body,sizeof(body)));
    assert(posts==1 && strstr(sent,"\"value\":true"));
    assert(c->value_valid && c->value==1 && c->observation==2 && d->power_on);
    assert(s->pending[0].dispatch_observation == 1);
    /* Unchanged cached desired value cannot confirm a failed post-write read. */
    get_error=-ETIMEDOUT;
    assert(!execute_control(s,&s->pending[0],body,sizeof(body)));
    assert(!c->value_valid && c->observation==2 && !d->power_valid);
    get_error=0;
    assert(!refresh_device_values(s,"test",body,sizeof(body)) && c->value_valid);
    int previous_reads=reads;
    post_body="{\"code\":0,\"data\":{\"results\":[{\"siid\":9,\"piid\":6,\"code\":-702000000}]}}";
    assert(execute_control(s,&s->pending[0],body,sizeof(body))==AGENT_OK);
    assert(reads==++previous_reads && c->value_valid);
    post_body="{\"code\":0,\"data\":{\"results\":[{}]}}";
    assert(execute_control(s,&s->pending[0],body,sizeof(body))==SMART_HOME_MILOCO_RESULT_UNCONFIRMED);
    assert(reads==++previous_reads && c->value_valid && c->value==1);
    post_error=-ETIMEDOUT;
    assert(execute_control(s,&s->pending[0],body,sizeof(body))==SMART_HOME_MILOCO_RESULT_UNCONFIRMED);
    assert(reads==++previous_reads && c->value_valid);
    post_error=0;
    post_body="{\"code\":0,\"data\":{\"results\":[{\"code\":-1}]}}";
    assert(execute_control(s,&s->pending[0],body,sizeof(body))!=0);
    assert(!c->value_valid && reads==previous_reads);
    int previous=posts;
    s->pending[0].value=3;
    assert(execute_control(s,&s->pending[0],body,sizeof(body))!=0 && posts==previous);
    sem_destroy(&s->wake); pthread_mutex_destroy(&s->lock); free(s);
    puts("PASS: real submit/dispatch/readback, model-specific power IID, POST bool, failed read invalidation, rejected write and constraints");
}
