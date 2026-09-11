/* Accelerometer — accelerometer_sample.c
 *
 * Purpose: hold the latest validated reading so other modules can query
 * it without touching I2C directly.
 */
#include "accelerometer_internal.h"

static AccelSample s_latest;
static bool s_has_sample = false;

void accelerometer_sample_store(const AccelSample *sample) {
    s_latest = *sample;
    s_has_sample = true;
}

bool accelerometer_get_sample(AccelSample *out) {
    if (!s_has_sample) {
        return false;
    }
    *out = s_latest;
    return true;
}
