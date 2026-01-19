# Define "phony" targets, which are not real files but names of commands to be executed.
# This prevents conflicts with files of the same name and improves performance.
.PHONY: all check clean

# The name of the final executable binary
TARGET = sehttpd

# A marker file to ensure git hooks are installed
GIT_HOOKS := .git/hooks/applied

# The default target 'all' depends on git hooks being installed and the target binary
all: $(GIT_HOOKS) $(TARGET)

# Rule to install git hooks if they haven't been applied yet
$(GIT_HOOKS):
	@scripts/install-git-hooks
	@echo

# Include common makefile definitions (like OS detection, verbosity settings)
include common.mk

# Compiler flags
# -I./src: Include header files from the ./src directory
CFLAGS = -I./src
# -O2: Optimize the code (level 2) for performance
CFLAGS += -O2
# -std=gnu99: Use the GNU99 C standard
# -Wall: Enable all common warnings
# -W: Enable extra warnings
CFLAGS += -std=gnu99 -Wall -W
# -DUNUSED="...": Define a macro UNUSED to suppress unused variable warnings
CFLAGS += -DUNUSED="__attribute__((unused))"
# -DNDEBUG: Define NDEBUG to disable assertions (and debug logs in this project)
CFLAGS += -DNDEBUG

# Linker flags (currently empty)
LDFLAGS =

# Standard build rules
# Define suffixes used in inference rules
.SUFFIXES: .o .c

# Implicit rule: How to build a .o (object) file from a .c (source) file
# $@ matches the target (the .o file)
# $< matches the first dependency (the .c file)
# -MMD -MF $@.d: Generate dependency files (.d) automatically.
#                This tracks which header files a source file includes, so
#                Make knows to rebuild the object file if a header changes.
.c.o:
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -c -MMD -MF $@.d $<

# List of object files needed to build the target
OBJS = \
    src/http.o \
    src/http_parser.o \
    src/http_request.o \
    src/timer.o \
    src/mainloop.o

# Add dependency files (.d) to the list of dependencies to track
deps += $(OBJS:%.o=%.o.d)

# Rule to link the object files into the final executable
# $^ matches all dependencies (all the object files)
$(TARGET): $(OBJS)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(CC) -o $@ $^ $(LDFLAGS)

# Rule to run tests
check: all
	@scripts/test.sh

# Rule to clean up build artifacts (executable, object files, dependency files)
clean:
	$(VECHO) "  Cleaning...\n"
	$(Q)$(RM) $(TARGET) $(OBJS) $(deps)

# Include the generated dependency files.
# The dash (-) at the beginning suppresses errors if the files don't exist yet.
-include $(deps)
