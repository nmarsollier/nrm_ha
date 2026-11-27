# NRM-HA — host test harness.
#
#   make build     compile the production C (mount + motors scheduler/queue/
#                  motion loop) into a shared library with RTOS/RMT/time mocked
#   make test      run the host test suite (Python unittest)

CC      ?= cc
PYTHON  ?= python3
HOSTDIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
ROOT    := $(HOSTDIR)../..

# Mount profile — nrm_ha (default) or bft.
PROFILE ?= nrm_ha

STUB_INC := -I$(HOSTDIR)stubs -I$(HOSTDIR)
PROD_INC := -I$(ROOT)/main -I$(ROOT)/main/mount \
            -I$(ROOT)/main/motors -I$(ROOT)/main/tmc -I$(ROOT)/main/utils \
            -I$(ROOT)/main/profiles -I$(ROOT)/main/geometry -I$(ROOT)/main/motion

CFLAGS := -shared -fPIC -std=c11 -O2 -Wall -Wextra $(STUB_INC) $(PROD_INC)

MOTOR_SRCS := \
  motors_current_state.c motors_enter_error_state.c motors_get_tracking_speed.c \
  motors_get_move_axis_speed.c motors_move_axis_speed.c motors_start_tracking.c \
  motors_stop.c motors_park.c motors_unpark.c motors_tools.c motors_hw.c motors_rmt.c \
  motors_command_queue.c motors_motion_task.c motors_pulse_guide.c \
  motors_is_pulse_guiding.c motors_microsteps.c motors_power.c

MOUNT_SRCS := \
  mount_tools.c mount_pulse_guide.c mount_set_move_axis_speed.c \
  mount_stop.c mount_park.c mount_unpark.c mount_set_tracking.c

PROD_SRCS := $(addprefix $(ROOT)/main/motors/,$(MOTOR_SRCS)) \
             $(addprefix $(ROOT)/main/mount/,$(MOUNT_SRCS)) \
             $(ROOT)/main/motion/axis_units.c \
             $(ROOT)/main/motion/slew_ramp.c \
             $(ROOT)/main/geometry/geometry_common.c \
             $(ROOT)/main/profiles/profile_$(PROFILE).c \
             $(ROOT)/main/profiles/mount_profile_validate.c

HOST_SRCS := $(HOSTDIR)host_main.c $(HOSTDIR)host_rtos.c $(HOSTDIR)host_rmt.c \
             $(HOSTDIR)host_gpio.c $(HOSTDIR)host_time.c $(HOSTDIR)tmc_stubs.c

# One library per profile, so `make PROFILE=bft test` never reuses NRM's build.
LIB := $(HOSTDIR)libhost_$(PROFILE).so

.PHONY: build test test-all clean

build: $(LIB)

$(LIB): $(PROD_SRCS) $(HOST_SRCS) $(HOSTDIR)host_internal.h
	$(CC) $(CFLAGS) $(PROD_SRCS) $(HOST_SRCS) -lpthread -lm -o $(LIB)

test: build
	NRM_TEST_PROFILE=$(PROFILE) PYTHONDONTWRITEBYTECODE=1 $(PYTHON) -m unittest discover -s $(HOSTDIR) -v

# Run both profiles.  Each runs the full host suite: the common motion suite
# (queue, stop barrier, RMT, tracking, move-axis, slew ramp) plus the
# per-profile geometry tests, all against the selected profile's scale/polarity.
test-all:
	$(MAKE) PROFILE=nrm_ha clean test
	$(MAKE) PROFILE=bft clean test

clean:
	rm -f $(HOSTDIR)/libhost_*.so
