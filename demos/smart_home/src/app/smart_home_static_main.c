/**
 * P4X P2 static Smart Home entry point.
 *
 * This deliberately does not initialize the normal smart_home agent,
 * networking, credentials, MCP, Node gateway, mobile App bridge, or touch
 * input.  It is the isolated LVGL + /dev/fb0 visual bring-up application.
 */

#include "../ui/lvgl/smart_home_lvgl_static.h"

#include <stdio.h>

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("=== Smart Home Static LVGL P2 ===\n");
    printf("starting local dashboard without network, cAGENT, or touch\n\n");
    return smart_home_lvgl_static_run();
}
