/* SPDX-License-Identifier: Apache-2.0 */
#include "smart_home_miloco_protocol.h"
#include "smart_home_miloco_specscan.h"
#include <agent/error.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static smart_home_miloco_device_t parse(const char *json)
{
    smart_home_miloco_device_t d = {0};
    assert(!smart_home_miloco_specscan(json, d.did, sizeof(d.did), &d.category,
        d.controls, SMART_HOME_MILOCO_MAX_CONTROLS, &d.control_count, &d.controllable));
    return d;
}

static void status(smart_home_miloco_device_t *d, const char *json)
{
    bool changed;
    assert(!smart_home_miloco_status_apply(d, json, &changed));
}

int main(void)
{
    bool changed;
    smart_home_miloco_device_t camera = parse(
        "{\"code\":0,\"data\":{\"did\":\"camera\",\"category\":\"camera\",\"spec\":{"
        "\"prop.2.6\":{\"description\":\"宽动态\",\"type_name\":\"wdr-mode\",\"format\":\"bool\",\"writeable\":true},"
        "\"prop.9.8\":{\"type_name\":\"on\",\"format\":\"bool\",\"writeable\":true}}}}");
    assert(camera.control_count == 2 && camera.controllable);
    assert(smart_home_miloco_power_index(&camera) == 1);
    status(&camera, "{\"code\":0,\"data\":{\"properties\":[{\"iid\":\"prop.2.6\",\"value\":true,\"code\":0},{\"iid\":\"prop.9.8\",\"value\":false,\"code\":0}]}}");
    assert(camera.controls[0].value_valid && camera.controls[0].value == 1);
    assert(camera.power_valid && !camera.power_on);
    uint32_t observation = camera.controls[0].observation;
    status(&camera, "{\"code\":0,\"data\":[{\"iid\":\"prop.2.6\",\"value\":1}]}");
    assert(camera.controls[0].observation != observation && !camera.power_valid);
    status(&camera, "{\"code\":0,\"data\":{\"properties\":[{\"iid\":\"prop.2.6\",\"value\":false,\"code\":0}]}}");
    assert(camera.controls[0].value_valid && camera.controls[0].value == 0);
    const char *bad[] = {
        "{\"code\":0,\"data\":{\"properties\":[{\"iid\":\"prop.2.6\",\"value\":true,\"code\":-1}]}}",
        "{\"code\":0,\"data\":[{\"iid\":\"prop.2.6\",\"value\":\"true\"}]}",
        "{\"code\":0,\"data\":[{\"iid\":\"prop.2.6\",\"value\":2}]}",
        "{\"code\":0,\"data\":[{\"iid\":\"prop.2.6\",\"value\":true},{\"iid\":\"prop.2.6\",\"value\":false}]}",
        "{\"code\":0,\"data\":[]}", "{\"code\":7,\"data\":[]}", "{}", "{", NULL
    };
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        (void)smart_home_miloco_status_apply(&camera, bad[i], &changed);
        assert(!camera.controls[0].value_valid);
        assert(camera.controls[0].value == 0); /* No corrupt defaults become state. */
    }
    smart_home_miloco_device_t light = parse(
        "{\"code\":0,\"data\":{\"did\":\"light\",\"category\":\"light\",\"spec\":{"
        "\"prop.7.4\":{\"format\":\"uint16\",\"writeable\":true,\"value_range\":[0.0,100.0,5e0]},"
        "\"prop.7.2\":{\"type_name\":\"on\",\"format\":\"bool\",\"writeable\":true}}}}");
    assert(light.control_count == 2 && smart_home_miloco_power_index(&light) == 1);
    status(&light,"{\"code\":0,\"data\":[{\"iid\":\"prop.7.4\",\"value\":35}]}");
    assert(light.controls[0].value_valid && light.controls[0].value == 35);
    status(&light,"{\"code\":0,\"data\":[{\"iid\":\"prop.7.4\",\"value\":36}]}");
    assert(!light.controls[0].value_valid);
    smart_home_miloco_device_t fan = parse(
        "{\"code\":0,\"data\":{\"did\":\"fan\",\"spec\":{"
        "\"prop.6.8\":{\"format\":\"uint8\",\"writeable\":true,\"value_list\":[{\"name\":\"low\",\"value\":1},{\"name\":\"high\",\"value\":7}]},"
        "\"prop.6.9\":{\"format\":\"bool\",\"writeable\":true,\"readable\":false}}}}");
    assert(fan.control_count == 2 && fan.controls[1].write_only);
    status(&fan,"{\"code\":0,\"data\":[{\"iid\":\"prop.6.8\",\"value\":7}]}");
    assert(fan.controls[0].value_valid && fan.controls[0].value == 7);
    status(&fan,"{\"code\":0,\"data\":[{\"iid\":\"prop.6.8\",\"value\":1.5}]}");
    assert(!fan.controls[0].value_valid);
    smart_home_miloco_device_t ac = parse(
        "{\"code\":0,\"data\":{\"did\":\"ac\",\"spec\":{"
        "\"prop.8.3\":{\"format\":\"int32\",\"writeable\":true,\"value_range\":[16,30,1]},"
        "\"action.8.1\":{\"in_params\":[]},\"action.8.2\":{\"in_params\":[{\"format\":\"string\"}]},"
        "\"prop.8.4\":{\"format\":\"float\",\"writeable\":true,\"value_range\":[16,30,0.5]},"
        "\"prop.8.5\":{\"format\":\"string\",\"writeable\":true}}}}");
    assert(ac.control_count == 2 && ac.controls[1].type == SMART_HOME_MILOCO_CTRL_ACTION);
    status(&ac,"{\"code\":0,\"data\":[{\"iid\":\"prop.8.3\",\"value\":24}]}");
    assert(ac.controls[0].value_valid && ac.controls[0].value == 24);
    camera.controls[0].type = SMART_HOME_MILOCO_CTRL_BOOL;
    strcpy(camera.controls[0].type_name, "on");
    assert(smart_home_miloco_power_index(&camera) < 0); /* ambiguous on != main power */
    smart_home_miloco_device_t invalid = parse(
        "{\"code\":0,\"data\":{\"did\":\"bad\",\"spec\":{"
        "\"prop.1.1\":{\"writeable\":true,\"value_list\":[{\"name\":\"a\",\"value\":0},{\"name\":\"b\",\"value\":1},{\"name\":\"c\",\"value\":2},{\"name\":\"d\",\"value\":3},{\"name\":\"e\",\"value\":4}]},"
        "\"prop.1.2\":{\"writeable\":true,\"value_list\":[{\"name\":\"a\",\"value\":0},{\"name\":\"b\",\"value\":0}]},"
        "\"prop.1.3\":{\"writeable\":true,\"value_list\":[{\"name\":\"a\",\"value\":\"str\"}]},"
        "\"action.1.1\":{\"type_name\":\"format\"}}}}");
    assert(invalid.control_count == 1 && invalid.controls[0].option_count == 5);
    char wide[4096] = "{\"code\":0,\"data\":{\"did\":\"many\",\"spec\":{";
    for (unsigned i = 0; i < SMART_HOME_MILOCO_MAX_CONTROLS; i++) {
        size_t used = strlen(wide);
        snprintf(wide + used, sizeof(wide)-used,
            "%s\"prop.9.%u\":{\"format\":\"bool\",\"writeable\":true}", i ? "," : "", i+1);
    }
    strcat(wide,"}}}");
    smart_home_miloco_device_t many = parse(wide);
    assert(many.control_count == SMART_HOME_MILOCO_MAX_CONTROLS);
    strcpy(wide,"{\"code\":0,\"data\":{\"did\":\"options\",\"spec\":{\"prop.9.1\":{\"writeable\":true,\"value_list\":[");
    for (unsigned i = 0; i <= SMART_HOME_MILOCO_MAX_OPTIONS; i++) {
        size_t used = strlen(wide);
        snprintf(wide+used,sizeof(wide)-used,"%s{\"name\":\"n%u\",\"value\":%u}",i?",":"",i,i*3);
    }
    strcat(wide,"]}}}}");
    many = parse(wide);
    assert(!many.control_count); /* Never silently cut enum options. */
    assert(!smart_home_miloco_control_reply("{\"code\":0,\"data\":{\"results\":[{\"did\":\"camera\",\"siid\":2,\"piid\":6,\"code\":0}]}}", "camera", "prop.2.6", false));
    assert(smart_home_miloco_control_reply("{\"code\":0,\"data\":{\"results\":[{\"code\":-704220043}]}}", "camera", "prop.2.6", false) != 0);
    assert(smart_home_miloco_control_reply("{\"code\":0,\"data\":{\"results\":[{\"piid\":1,\"code\":0}]}}", "camera", "prop.2.6", false) != 0);
    assert(smart_home_miloco_control_reply("{\"code\":0}", "camera", "prop.2.6", false) != 0);
    assert(!smart_home_miloco_control_reply("{\"code\":0,\"data\":{\"result\":{\"siid\":8,\"aiid\":1,\"code\":0}}}", "ac", "action.8.1", true));
    puts("PASS: camera/light/fan/AC, nested/legacy status, bool/int/enum/range, per-item errors, duplicates, freshness, capabilities and control results");
}
