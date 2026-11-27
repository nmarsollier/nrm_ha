/* Proto — proto_capabilities.c — CAPABILITIES: unified handshake + device-specific capabilities.
 *
 * The response uses the shared identification header (device_type, device_id,
 * name, firmware_version, proto, boot_id) so a client can tell the mount from
 * the accessories controller with one parser. The `capabilities` object then
 * holds the mount-specific capabilities: the geometry, the mechanical scale,
 * the speed tables, the slew ramp and the mechanical/default limits — all read
 * directly from `mount_profile`, so the client can derive every expectation
 * without a second source of constants.
 */
#include "proto_internal.h"

#include "mount_profile.h"

/* Firmware version reported in the CAPABILITIES handshake.  Kept separate from
 * the mount profile (mount_profile.h): it describes the firmware build, not the
 * board. */
#define MOUNT_FIRMWARE_VERSION "0.1.0" /* TODO: bump to the real version */

void proto_handle_capabilities(const char *req, uint32_t id, char *out, size_t cap) {
    (void) req;

    size_t len = 0;
    out[0] = '\0';

    proto_buf_append(out, cap, &len,
        "{\"type\":\"capabilities\",\"id\":%lu,\"ok\":true,"
        "\"boot_id\":\"%08lx\",\"proto\":1,"
        "\"device_type\":\"mount\",\"device_id\":\"%s\","
        "\"name\":\"%s\",\"firmware_version\":\"%s\","
        "\"capabilities\":{\"axes\":[\"ra\",\"dec\"],"
        "\"geometry\":\"%s\","
        "\"gear_ratio\":%.4f,\"full_step_deg\":%.4f,"
        "\"canonical_microsteps\":%u,\"slew_microsteps\":%u,"
        "\"slew_speeds_dps\":[",
        (unsigned long) id, (unsigned long) proto_boot_id(),
        mount_profile.id, mount_profile.name, MOUNT_FIRMWARE_VERSION,
        (mount_profile.geometry == GEOMETRY_GERMAN) ? "german" : "fork",
        mount_profile.gear_ratio, mount_profile.full_step_deg,
        (unsigned) mount_profile.canonical_microsteps,
        (unsigned) mount_profile.slew_microsteps);

    for (int rate = 1; rate <= 4; rate++) {
        proto_buf_append(out, cap, &len, "%s%.3f",
                         rate > 1 ? "," : "", motors_get_slewing_speed(rate));
    }

    proto_buf_append(out, cap, &len, "],\"move_axis_speeds_dps\":[");
    for (int rate = 1; rate <= 10; rate++) {
        proto_buf_append(out, cap, &len, "%s%.3f",
                         rate > 1 ? "," : "", motors_get_move_axis_speed(rate));
    }

    proto_buf_append(out, cap, &len,
        "],\"tracking_modes\":[\"none\",\"sidereal\",\"lunar\",\"solar\"],"
        "\"slew\":{\"min_speed_dps\":%.4f,\"short_distance_deg\":%.4f,"
        "\"gentle_distance_deg\":%.4f,\"gentle_max_dps\":%.4f,"
        "\"accel_distance_deg\":%.4f,\"decel_distance_deg\":%.4f},"
        "\"mechanical_limits_deg\":{\"ra_min\":%.4f,\"ra_max\":%.4f,"
        "\"dec_min\":%.4f,\"dec_max\":%.4f},"
        "\"default_limits_deg\":{\"ra_min\":%.4f,\"ra_max\":%.4f,"
        "\"dec_min\":%.4f,\"dec_max\":%.4f},"
        "\"caps\":{\"guide\":true,\"home\":true,\"park\":true,\"unpark\":true,"
        "\"move_axis\":true,\"slew\":true,\"tracking\":true,\"sync\":false,"
        "\"meridian_flip\":%s}"
        "}}",
        mount_profile.slew.min_speed_dps, mount_profile.slew.short_distance_deg,
        mount_profile.slew.gentle_distance_deg, mount_profile.slew.gentle_max_dps,
        mount_profile.slew.accel_distance_deg, mount_profile.slew.decel_distance_deg,
        mount_profile.axes[AXIS_RA].hard_min_deg, mount_profile.axes[AXIS_RA].hard_max_deg,
        mount_profile.axes[AXIS_DEC].hard_min_deg, mount_profile.axes[AXIS_DEC].hard_max_deg,
        mount_profile.axes[AXIS_RA].default_min_deg, mount_profile.axes[AXIS_RA].default_max_deg,
        mount_profile.axes[AXIS_DEC].default_min_deg, mount_profile.axes[AXIS_DEC].default_max_deg,
        (mount_profile.geometry == GEOMETRY_GERMAN) ? "true" : "false");
}
