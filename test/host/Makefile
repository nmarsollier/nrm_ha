# NRM-HA — host test harness.
#
#   make build     compile the production C (mount + motors scheduler/queue/
#                  motion loop) into a shared library with RTOS/RMT/time mocked
#   make test      run the host test suite (Python unittest)

CC      ?= cc
PYTHON  ?= python3
HOSTDIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
ROOT    := $(HOSTDIR)../..

STUB_INC := -I$(HOSTDIR)stubs -I$(HOSTDIR)
PROD_INC := -I$(ROOT)/main -I$(ROOT)/main/config -I$(ROOT)/main/mount \
            -I$(ROOT)/main/motors -I$(ROOT)/main/utils

CFLAGS := -shared -fPIC -std=c11 -O2 -Wall -Wextra $(STUB_INC) $(PROD_INC)

MOTOR_SRCS := \
  motors_current_state.c motors_enter_error_state.c motors_get_tracking_speed.c \
  motors_get_move_axis_speed.c motors_move_axis_speed.c motors_start_tracking.c \
  motors_stop.c motors_tools.c motors_hw.c motors_rmt.c motors_command_queue.c \
  motors_motion_task.c motors_pulse_guide.c motors_is_pulse_guiding.c \
  motors_power.c

MOUNT_SRCS := \
  mount_tools.c mount_pulse_guide.c mount_set_move_axis_speed.c \
  mount_stop.c mount_set_tracking.c

PROD_SRCS := $(addprefix $(ROOT)/main/motors/,$(MOTOR_SRCS)) \
             $(addprefix $(ROOT)/main/mount/,$(MOUNT_SRCS)) \
             $(ROOT)/main/utils/string_utils.c

HOST_SRCS := $(HOSTDIR)host_main.c $(HOSTDIR)host_rtos.c $(HOSTDIR)host_rmt.c \
             $(HOSTDIR)host_gpio.c $(HOSTDIR)host_time.c

LIB := $(HOSTDIR)libnrm_host.so

.PHONY: build test clean

build: $(LIB)

$(LIB): $(PROD_SRCS) $(HOST_SRCS) $(HOSTDIR)host_internal.h
	$(CC) $(CFLAGS) $(PROD_SRCS) $(HOST_SRCS) -lpthread -lm -o $(LIB)

test: build
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) -m unittest discover -s $(HOSTDIR) -v

clean:
	rm -f $(LIB)
