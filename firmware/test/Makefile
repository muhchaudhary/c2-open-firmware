# firmware/test: host unit tests for the hardware-independent firmware code.
#
#   make -C firmware/test            build and run them all
#   make -C firmware/test test_link  one (build/test_link)
#   TEST_VERBOSE=1 build/test_motion also shows the firmware's log lines
#
# Each test program is test_<name>.c + fake.c (the SDK / board pieces) +
# the firmware files in src_<name>, so every program starts with fresh
# firmware state. shim/ stands in for the SDK headers. Built with ASan and
# UBSan (SANITIZE= to build without).
CC       ?= cc
SRC      := ../src
OUT      := build
SANITIZE ?= -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
CFLAGS   ?= -O1 -g
CFLAGS   += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers \
            -Werror $(SANITIZE) -Ishim -I$(SRC)/core -I$(SRC)/drivers
LDLIBS   := -lm

TESTS := link sinput xinput ds4 settings tuning stick_axis hall_trigger motion battery

src_link         := core/gp_link.c
src_sinput       := core/gp_sinput_desc.c
src_xinput       := core/gp_xinput_desc.c
src_ds4          := core/gp_ds4_desc.c
src_settings     := core/gp_settings.c
src_tuning       := core/gp_tuning.c core/gp_settings.c
src_stick_axis   := drivers/stick_axis.c
src_hall_trigger := drivers/hall_trigger.c
src_motion       := core/gp_motion.c
src_battery      := drivers/battery_adc.c

BINS := $(addprefix $(OUT)/test_,$(TESTS))

.PHONY: all run clean $(addprefix test_,$(TESTS))
all: run

run: $(BINS)
	@fail=0; for t in $(BINS); do $$t || fail=1; done; \
	if [ $$fail = 0 ]; then echo "all host tests passed"; else echo "HOST TESTS FAILED"; exit 1; fi

$(addprefix test_,$(TESTS)): test_%: $(OUT)/test_%
	@$<

.SECONDEXPANSION:
$(OUT)/test_%: test_%.c fake.c test.h $$(addprefix $(SRC)/,$$(src_$$*)) $$(wildcard shim/*.h shim/*/*.h shim/*/*/*.h)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ test_$*.c fake.c $(addprefix $(SRC)/,$(src_$*)) $(LDLIBS)

clean:
	rm -rf $(OUT)
