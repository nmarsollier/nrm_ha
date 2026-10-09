/* Proto — proto_capabilities.c — CAPABILITIES: identity and static capabilities. */
#include "proto_internal.h"

#include "config.h"

void proto_handle_capabilities(const char *req, uint32_t id, char *out, size_t cap) {
    (void) req;

    size_t len = 0;
    out[0] = '\0';

    proto_buf_append(out, cap, &len,
        "{\"type\":\"capabilities\",\"id\":%lu,\"ok\":true,"
        "\"boot_id\":%lu,\"proto\":1,\"name\":\"%s\","
        "\"axes\":[\"ra\",\"dec\"],\"slew_speeds_dps\":[",
        (unsigned long) id, (unsigned long) proto_boot_id(), MOUNT_NAME);

    for (int rate = 1; rate <= 4; rate++) {
        proto_buf_append(out, cap, &len, "%s%.1f",
                         rate > 1 ? "," : "", motors_get_slewing_speed(rate));
    }

    proto_buf_append(out, cap, &len,
        "],\"tracking_modes\":[\"none\",\"sidereal\",\"lunar\",\"solar\"],"
        "\"caps\":{\"guide\":true,\"home\":true,\"park\":true,\"unpark\":true,"
        "\"move_axis\":true,\"slew\":true,\"tracking\":true,\"sync\":false},"
        "\"config_rev\":%lu}",
        (unsigned long) proto_config_rev());
}
