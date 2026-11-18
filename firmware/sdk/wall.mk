# wall.mk: our sources with -Wall, on top of the SDK's Makefile (which builds
# everything with -w). Syntax only, nothing is written; any warning fails.
#
#   make -C SDK/apps/hid/board/br23 -f Makefile -f $PWD/firmware/sdk/wall.mk wall TOOL_DIR=...
#
# firmware/build.sh --wall runs it after the build (CI does).
WALL_SRCS  := $(filter $(ROOT_PREFIX)/apps/gamepad/%,$(c_SRC_FILES))
WALL_FLAGS := $(filter-out -w -flto,$(CFLAGS)) -Wall -Wextra -Wno-unused-parameter \
              -Wno-sign-compare -Wno-missing-field-initializers -Wno-builtin-macro-redefined -fsyntax-only
# The SDK's headers as system headers (their warnings aren't ours), ours as they are.
WALL_INCS  := $(foreach i,$(INCLUDES),$(if $(findstring /apps/gamepad/,$(i)),$(i),$(patsubst -I%,-isystem %,$(i))))

.PHONY: wall
wall:
	@n=0; for f in $(WALL_SRCS); do \
	  out=$$($(CC) $(WALL_FLAGS) $(DEFINES) $(WALL_INCS) $$f 2>&1) || n=$$((n+1)); \
	  if [ -n "$$out" ]; then echo "$$out"; n=$$((n+1)); fi; \
	done; \
	echo "wall: $(words $(WALL_SRCS)) files, $$n with warnings"; [ $$n = 0 ]
