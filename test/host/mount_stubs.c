/* Host — stubs for the coordinate/guide fork test.
 *
 * Compiles the production mount coordinate/guide math (mount_coordinate_tools.c,
 * mount_get_lst.c, mount_pulse_guide.c, mount_slew_to_coordinates.c) with the
 * motors layer and the clock mocked, so the pure conversions and the pier-side
 * selection can be verified against an independent oracle without hardware.
 *
 * The clock is overridden via -Dgettimeofday=host_gettimeofday and
 * -Dtime=host_time (see test_fork.py), so `axis_to_equatorial` and the GOTO
 * arrival compensation are deterministic for a fixed instant.
 */
#include "mount/mount_internal.h"
#include <sys/time.h>
#include <time.h>

MountSettings mount_internal_state;
bool mount_time_valid = true;

static MotorsState state;
static float dec_position;
static float ra_position;
static time_t clock_seconds;

int guide_axis;
float guide_offset;
uint32_t guide_duration;

/* Last slew recorded by motors_slew_to_angle. */
int slew_called;
float slew_ra;
float slew_dec;

void host_reset(time_t now) {
    clock_seconds = now;
    mount_internal_state = (MountSettings){.lat = -32.89f, .lon = 0};
    mount_time_valid = true;
    state = (MotorsState){.status = MOTORS_STATUS_READY,
        .limits = {-150, 150, -150, 150}};
    dec_position = 0;
    ra_position = 0;
    guide_axis = -1;
    slew_called = 0;
}
void host_lat(float lat) { mount_internal_state.lat = lat; }
void host_lon(float lon) { mount_internal_state.lon = lon; }
void host_time_valid(int v) { mount_time_valid = v ? true : false; }
void host_limits(float ra_min, float ra_max, float dec_min, float dec_max) {
    state.limits.ra_min = ra_min; state.limits.ra_max = ra_max;
    state.limits.dec_min = dec_min; state.limits.dec_max = dec_max;
}
void host_dec(float dec) { dec_position = dec; }
void host_ra(float ra) { ra_position = ra; }
void host_set_time(double t) { clock_seconds = (time_t) t; }

int host_gettimeofday(struct timeval *tv, void *tz) {
    (void) tz;
    tv->tv_sec = clock_seconds;
    tv->tv_usec = 0;
    return 0;
}
time_t host_time(time_t *t) {
    if (t != NULL) *t = clock_seconds;
    return clock_seconds;
}

MotorsState motors_current_state(void) { return state; }
float motors_get_dec_deg(void) { return dec_position; }
float motors_get_ra_deg(void) { return ra_position; }
float motors_get_slewing_speed(int speed_rate) {
    switch (speed_rate) {
        case 1: return 0.5f;
        case 2: return 1.0f;
        case 3: return 2.5f;
        default: return 4.0f;
    }
}
/* Test-only approximation — the real ramp model lives in the firmware
 * (motors_slew_duration.c), which this fork harness does not compile. */
float motors_slew_duration_seconds(float distance_deg, float speed_dps) {
    return (distance_deg > 0.0f && speed_dps > 0.0f)
           ? distance_deg / speed_dps + 2.0f
           : 0.0f;
}
bool motors_status_is_error(MotorsStatus s) { return s == MOTORS_STATUS_ERROR; }
const char *motors_status_to_string(MotorsStatus s) { (void) s; return "ready"; }
MountResult mount_result_error(const char *msg) { return (MountResult){false, msg}; }
MountResult mount_result_ok(void) { return (MountResult){true, "OK"}; }
MountResult motors_result_code_error_result(MotorResultCode rc) {
    return (MountResult){rc == MOTOR_OK, ""};
}
MotorResultCode motors_slew_to_angle(float ra_deg, float dec_deg, int speed_rate) {
    (void) speed_rate;
    slew_called = 1;
    slew_ra = ra_deg;
    slew_dec = dec_deg;
    return MOTOR_OK;
}
MotorResultCode motors_pulse_guide_start(int axis, float offset, uint32_t duration) {
    guide_axis = axis; guide_offset = offset; guide_duration = duration;
    return MOTOR_OK;
}

/* Simplify the ABI used by the Python tests. */
bool host_guide(int direction) { return mount_pulse_guide(direction, 500).ok; }
bool host_slew(float ra, float dec, int speed) {
    return mount_slew_to_coordinates(ra, dec, speed, PIER_SIDE_AUTO).ok;
}
/* Predict the pier side a goto would select (0 east, 1 west), or -1 when
 * unreachable. */
int host_destination_side_of_pier(float ra, float dec) {
    int side = -1;
    MountResult r = mount_destination_side_of_pier(ra, dec, &side);
    return r.ok ? side : -1;
}
