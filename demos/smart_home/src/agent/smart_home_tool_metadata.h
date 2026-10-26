#pragma once

#include <stdint.h>
#include <agent.h>
#include <string.h>

/* UI IDs remain application-owned; V2 registrations use the string names. */
#define SMART_HOME_TOOL_GROUP_LOCAL 0u
#define SMART_HOME_TOOL_GROUP_NODE  10u
#define SMART_HOME_TOOL_GROUP_MCP   20u
#define SMART_HOME_TOOL_GROUP_UNKNOWN UINT16_MAX

#define SMART_HOME_TOOL_GROUP_LOCAL_NAME "local"
#define SMART_HOME_TOOL_GROUP_NODE_NAME  "node"
#define SMART_HOME_TOOL_GROUP_MCP_NAME   "mcp"
#define SMART_HOME_TOOL_CATEGORY_QUERY_NAME "query"
#define SMART_HOME_TOOL_CATEGORY_CONTROL_NAME "control"
#define SMART_HOME_TOOL_CATEGORY_SKILL_NAME "skill"

/* SmartHome settings-page categories. */
typedef enum {
    SMART_HOME_TOOL_CATEGORY_UNSPECIFIED = 0,
    SMART_HOME_TOOL_CATEGORY_QUERY = 1,
    SMART_HOME_TOOL_CATEGORY_CONTROL = 2,
    SMART_HOME_TOOL_CATEGORY_SKILL = 3,
} smart_home_tool_category_t;

typedef struct {
    const char *name;
    const char *description;
    uint16_t group_id;
    uint16_t category_id;
    uint32_t flags; /* Native V2 flags. */
    int enabled;
    int llm_visible;
    int access_allowed;
} smart_home_tool_info_t;

/* Strings expire when the visitor returns. */
typedef int (*smart_home_tool_visit_fn)(const smart_home_tool_info_t *tool,
                                       void *user_data);

static inline int smart_home_tool_text_equal(agent_string_view_t text,
                                             const char *literal)
{
    size_t size = strlen(literal);
    return text.size == size && (size == 0u ||
           (text.data && memcmp(text.data, literal, size) == 0));
}

static inline uint16_t smart_home_tool_group_id(agent_string_view_t group)
{
    if (group.size == 0u || smart_home_tool_text_equal(group, SMART_HOME_TOOL_GROUP_LOCAL_NAME))
        return SMART_HOME_TOOL_GROUP_LOCAL;
    if (smart_home_tool_text_equal(group, SMART_HOME_TOOL_GROUP_NODE_NAME))
        return SMART_HOME_TOOL_GROUP_NODE;
    if (smart_home_tool_text_equal(group, SMART_HOME_TOOL_GROUP_MCP_NAME))
        return SMART_HOME_TOOL_GROUP_MCP;
    return SMART_HOME_TOOL_GROUP_UNKNOWN;
}

static inline uint16_t smart_home_tool_category_id(agent_string_view_t category)
{
    if (smart_home_tool_text_equal(category, SMART_HOME_TOOL_CATEGORY_QUERY_NAME))
        return SMART_HOME_TOOL_CATEGORY_QUERY;
    if (smart_home_tool_text_equal(category, SMART_HOME_TOOL_CATEGORY_CONTROL_NAME))
        return SMART_HOME_TOOL_CATEGORY_CONTROL;
    if (smart_home_tool_text_equal(category, SMART_HOME_TOOL_CATEGORY_SKILL_NAME))
        return SMART_HOME_TOOL_CATEGORY_SKILL;
    return SMART_HOME_TOOL_CATEGORY_UNSPECIFIED;
}
