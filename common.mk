# Detect the operating system using the 'uname' command
UNAME_S := $(shell uname -s)

# macOS (Darwin) and Linux have slightly different printf implementations.
# This ensures consistent output formatting across platforms.
ifeq ($(UNAME_S),Darwin)
    PRINTF = printf
else
    PRINTF = env printf
endif

# Control the build verbosity
# If VERBOSE=1 is passed to make, show the full commands being executed.
# Otherwise, show a simplified output (e.g., "CC src/http.o").
ifeq ("$(VERBOSE)","1")
    # Q is used to prefix commands. If empty, the command is printed by make.
    Q :=
    # VECHO is used to print the simplified status. If verbose, we silence it.
    VECHO = @true
else
    # @ prefix suppresses the command echo in make
    Q := @
    # VECHO prints the simplified status message
    VECHO = @$(PRINTF)
endif

# Color definitions for terminal output (Green and Reset)
PASS_COLOR = \e[32;01m
NO_COLOR = \e[0m
