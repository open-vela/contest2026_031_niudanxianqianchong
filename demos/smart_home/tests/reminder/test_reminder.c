/* SPDX-License-Identifier: Apache-2.0 */
/* Deterministic clock tests exercise the same engine used by the worker. */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../src/reminder/smart_home_reminder.c"

static atomic_llong fake_wall, fake_mono;
static int64_t read_wall(void *arg) { (void)arg; return atomic_load(&fake_wall); }
static int64_t read_mono(void *arg) { (void)arg; return atomic_load(&fake_mono); }

static reminder_command_t command(int delay, int64_t at)
{
    reminder_command_t cmd = {.kind=1,.delay=delay,.at=at};
    strcpy(cmd.name,"提醒");
    strcpy(cmd.message,"喝水 \"测试\"\n第二行");
    return cmd;
}

static void engine_tests(const char *path)
{
    const int64_t now = INT64_C(1793718000);
    reminder_engine_t e = {0}, restart = {0};
    reminder_command_t cmd;
    int64_t at;
    int i, id;
    strcpy(e.path,path);
    assert(engine_open(&e) == AGENT_OK);
    assert(smart_home_reminder_parse_local("2028-02-29T08:00:00",&at) == AGENT_OK);
    assert(smart_home_reminder_parse_local("2027-02-29T08:00:00",&at) == AGENT_ERROR_INVALID);
    assert(smart_home_reminder_parse_local("2028-02-29T24:00:00",&at) == AGENT_ERROR_INVALID);
    assert(smart_home_reminder_parse_local("2028-02-29T08:00:00Z",&at) == AGENT_ERROR_INVALID);
    assert(smart_home_reminder_parse_local("2024-01-01T00:00:00",&at) == AGENT_OK);
    assert(at == SMART_HOME_REMINDER_MIN_EPOCH);
    assert(smart_home_reminder_parse_local("2038-01-01T00:00:00",&at) == AGENT_ERROR_INVALID);
    cmd = command(10,0);
    assert(engine_create(&e,&cmd,1000,100) == AGENT_ERROR_STATE);
    assert(!e.records[0].item.id);
    assert(engine_create(&e,&cmd,now,100) == AGENT_OK);
    id = cmd.id;
    assert(engine_tick(&e,now+3600,105) == AGENT_OK);
    assert(e.records[0].item.state == SMART_HOME_REMINDER_ACTIVE);
    assert(e.records[0].item.due_epoch == now+3605);
    assert(engine_tick(&e,now-3600,110) == AGENT_OK);
    assert(e.records[0].item.state == SMART_HOME_REMINDER_DUE);
    assert(e.records[0].item.audible);
    strcpy(restart.path,path);
    assert(engine_open(&restart) == AGENT_OK);
    assert(restart.records[0].item.state == SMART_HOME_REMINDER_DUE);
    assert(!restart.records[0].item.audible); /* no replayed ringing */
    assert(engine_remove(&e,id) == AGENT_OK);
    assert(engine_remove(&e,id) == AGENT_ERROR_NOT_FOUND);
    cmd = command(30,0);
    assert(engine_create(&e,&cmd,now,100) == AGENT_OK);
    memset(&restart,0,sizeof(restart)); strcpy(restart.path,path);
    assert(engine_open(&restart) == AGENT_OK);
    assert(engine_tick(&restart,now+10,500) == AGENT_OK);
    assert(restart.records[0].mono_due == 520);
    assert(engine_tick(&restart,now+30,520) == AGENT_OK);
    assert(restart.records[0].item.state == SMART_HOME_REMINDER_DUE);
    assert(engine_remove(&e,cmd.id) == AGENT_OK);
    cmd = command(0,now-1);
    assert(engine_create(&e,&cmd,now,100) == AGENT_ERROR_INVALID);
    cmd = command(0,now+10);
    assert(engine_create(&e,&cmd,now,100) == AGENT_OK);
    memset(&restart,0,sizeof(restart)); strcpy(restart.path,path);
    assert(engine_open(&restart) == AGENT_OK);
    assert(engine_tick(&restart,1000,200) == AGENT_OK);
    assert(restart.records[0].item.state == SMART_HOME_REMINDER_ACTIVE);
    assert(engine_tick(&restart,now+20,200) == AGENT_OK);
    assert(restart.records[0].item.state == SMART_HOME_REMINDER_DUE);
    assert(restart.records[0].item.audible);
    assert(save_records(&e) == AGENT_OK);
    memset(&restart,0,sizeof(restart)); strcpy(restart.path,path);
    assert(engine_open(&restart) == AGENT_OK);
    assert(engine_tick(&restart,now+1000,200) == AGENT_OK);
    assert(restart.records[0].item.state == SMART_HOME_REMINDER_MISSED);
    assert(!restart.records[0].item.audible);
    assert(engine_remove(&e,cmd.id) == AGENT_OK);
    for (i=0;i<SMART_HOME_REMINDER_MAX;++i) {
        cmd = command(60,0);
        assert(engine_create(&e,&cmd,now,100) == AGENT_OK);
    }
    cmd = command(1,0);
    assert(engine_create(&e,&cmd,now,100) == AGENT_ERROR_CAPACITY);
    /* Failed storage cannot manufacture a successful cancel/create/trigger. */
    snprintf(e.path,sizeof(e.path),"%s/no-parent/reminders.json",path);
    assert(engine_remove(&e,e.records[0].item.id) != AGENT_OK);
    assert(e.records[0].item.id);
    assert(engine_tick(&e,now+61,161) != AGENT_OK);
    assert(e.records[0].item.state == SMART_HOME_REMINDER_ACTIVE);
    memset(&e,0,sizeof(e)); e.next_id=1;
    snprintf(e.path,sizeof(e.path),"%s/no-parent/reminders.json",path);
    assert(engine_create(&e,&cmd,now,100) != AGENT_OK);
    assert(e.next_id == 1 && e.records[0].item.id == 0);
    unlink(path);
    {
        FILE *f=fopen(path,"w"); assert(f); fputs("{bad json",f); fclose(f);
        memset(&restart,0,sizeof(restart)); strcpy(restart.path,path);
        assert(engine_open(&restart) != AGENT_OK);
        f=fopen(path,"r"); assert(f);
        char preserved[32]={0};
        assert(fread(preserved,1,sizeof(preserved)-1,f)==9);
        assert(!strcmp(preserved,"{bad json")); fclose(f);
    }
    unlink(path);
    puts("PASS: dates, clock jumps, reboot, grace, capacity, storage failure, corrupt file");
}

static smart_home_reminder_snapshot_t await_count(int count)
{
    smart_home_reminder_snapshot_t snapshot;
    int i;
    for (i=0;i<200;++i) {
        smart_home_reminder_snapshot(&snapshot);
        if (snapshot.ready && snapshot.count==count) return snapshot;
        usleep(10000);
    }
    assert(!"worker did not publish expected state");
    return snapshot;
}

static void worker_tests(const char *path)
{
    smart_home_reminder_options_t options = {path,read_wall,read_mono,NULL};
    smart_home_reminder_snapshot_t snapshot;
    int id;
    atomic_store(&fake_wall,INT64_C(1793718000)); atomic_store(&fake_mono,100);
    assert(smart_home_reminder_start(&options) == AGENT_OK);
    assert(smart_home_reminder_start(&options) == AGENT_ERROR_BUSY);
    await_count(0);
    assert(smart_home_reminder_create(10,0,"水","喝水",&id) == AGENT_OK);
    snapshot=await_count(1);
    assert(snapshot.items[0].id==id);
    atomic_fetch_add(&fake_mono,11); atomic_fetch_add(&fake_wall,11);
    for (int i=0;i<200;++i) {
        snapshot=await_count(1);
        if (snapshot.items[0].state==SMART_HOME_REMINDER_DUE) break;
        usleep(10000);
    }
    assert(snapshot.items[0].state==SMART_HOME_REMINDER_DUE);
    smart_home_reminder_stop();
    assert(smart_home_reminder_start(&options)==AGENT_OK);
    snapshot=await_count(1);
    assert(snapshot.items[0].state==SMART_HOME_REMINDER_DUE && !snapshot.items[0].audible);
    assert(smart_home_reminder_ack(id)==AGENT_OK);
    await_count(0);
    smart_home_reminder_stop();
    assert(smart_home_reminder_start(&options)==AGENT_OK);
    await_count(0);
    assert(smart_home_reminder_create(60,0,NULL,"test",&id)==AGENT_OK);
    assert(smart_home_reminder_cancel(id)==AGENT_OK);
    assert(smart_home_reminder_cancel(id)==AGENT_ERROR_NOT_FOUND);
    smart_home_reminder_stop();
    unlink(path);
    puts("PASS: worker commands, cached notifications, async ack and durable restart");
}

int main(void)
{
    char dir[]="/tmp/reminder-test-XXXXXX", path[256];
    assert(mkdtemp(dir));
    snprintf(path,sizeof(path),"%s/reminders.json",dir);
    engine_tests(path);
    worker_tests(path);
    rmdir(dir);
    return 0;
}
