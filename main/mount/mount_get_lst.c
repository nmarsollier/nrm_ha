/* Mount - mount_get_lst.c
 *
 * Purpose: compute the current Local Sidereal Time for the configured site.
 * Delegates to the pure geometry time helper.
 */
#include "mount.h"
#include "mount_internal.h"

#include <sys/time.h>

#include "geometry/mount_geometry.h"

double mount_unix_now(void) {
    /* Fractional-second clock — avoids the ~15"/s sawtooth in reported RA. */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double) tv.tv_sec + (double) tv.tv_usec / 1e6;
}

float mount_get_lst(void) {
    return geometry_lst(mount_internal_state.lon, mount_unix_now());
}
