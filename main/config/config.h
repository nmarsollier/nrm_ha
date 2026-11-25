#pragma once

/*
 * Mount configuration adapter (transient).
 *
 * Business code keeps including "config.h" for build-time constants, but the
 * constants now live in two places selected by the build:
 *   - board/board_config.h      common electronics (pins, microsteps, currents)
 *   - profiles/mount_profile.h  mechanical data (identity, reduction, …)
 *
 * Do not add mount-specific values here — put electronics in board_config.h
 * and mechanical data in the selected profile (main/profiles/).
 */

#include "board/board_config.h"
#include "profiles/mount_profile.h"
