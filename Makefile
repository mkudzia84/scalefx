# Compiler settings
CC = gcc
CFLAGS = -Wall -Wextra -Werror -Wno-unused-function -std=c23 -D_DEFAULT_SOURCE
INCLUDES = -I./include
# Dependencies:
# - libcyaml (YAML parsing) depends on libyaml
# - pigpio (GPIO control) - requires pigpiod daemon running
# - Audio: ALSA libs (libasound2)
LIBS = -ldl -lm -latomic -lcyaml -lyaml -lpigpio -lrt -lpthread

# Build options
# Set to 0 to disable JetiEX telemetry support
ENABLE_JETIEX ?= 1

# Debug build: make DEBUG=1
DEBUG ?= 0
ifeq ($(DEBUG),1)
CFLAGS += -g -O0 -DDEBUG
else
CFLAGS += -O2
endif

# Directories
SRC_DIR = src
INCLUDE_DIR = include
BUILD_DIR = build
DEMO_DIR = demo
SCRIPTS_DIR = scripts

# Output binaries
HELIFX = $(BUILD_DIR)/helifx
DEMO_TARGETS = $(BUILD_DIR)/mixer_demo \
               $(BUILD_DIR)/engine_fx_demo $(BUILD_DIR)/gun_fx_demo \
               $(BUILD_DIR)/servo_demo

# Conditionally add JetiEX demo
ifeq ($(ENABLE_JETIEX),1)
DEMO_TARGETS += $(BUILD_DIR)/jetiex_demo
CFLAGS += -DENABLE_JETIEX
endif

# All targets
TARGETS = $(HELIFX) $(DEMO_TARGETS)

# Source files
HELIFX_SRCS = $(SRC_DIR)/main.c $(SRC_DIR)/config_loader.c \
              $(SRC_DIR)/engine_fx.c $(SRC_DIR)/gun_fx.c \
              $(SRC_DIR)/lights.c $(SRC_DIR)/smoke_generator.c \
              $(SRC_DIR)/audio_player.c $(SRC_DIR)/gpio.c $(SRC_DIR)/servo.c

# Conditionally add JetiEX sources
ifeq ($(ENABLE_JETIEX),1)
HELIFX_SRCS += $(SRC_DIR)/jetiex.c $(SRC_DIR)/helifx_jetiex.c
endif

MIXER_DEMO_SRCS = $(DEMO_DIR)/mixer_demo.c $(SRC_DIR)/audio_player.c $(SRC_DIR)/gpio.c
ENGINE_FX_DEMO_SRCS = $(DEMO_DIR)/engine_fx_demo.c $(SRC_DIR)/engine_fx.c \
                      $(SRC_DIR)/audio_player.c $(SRC_DIR)/gpio.c
GUN_FX_DEMO_SRCS = $(DEMO_DIR)/gun_fx_demo.c $(SRC_DIR)/gun_fx.c \
                   $(SRC_DIR)/lights.c $(SRC_DIR)/smoke_generator.c \
                   $(SRC_DIR)/audio_player.c $(SRC_DIR)/gpio.c $(SRC_DIR)/servo.c
SERVO_DEMO_SRCS = $(DEMO_DIR)/servo_demo.c $(SRC_DIR)/servo.c
JETIEX_DEMO_SRCS = $(DEMO_DIR)/jetiex_demo.c $(SRC_DIR)/jetiex.c

# Object files
HELIFX_OBJS = $(HELIFX_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
MIXER_DEMO_OBJS = $(BUILD_DIR)/demo/mixer_demo.o $(BUILD_DIR)/audio_player.o $(BUILD_DIR)/gpio.o
ENGINE_FX_DEMO_OBJS = $(BUILD_DIR)/demo/engine_fx_demo.o $(BUILD_DIR)/engine_fx.o \
                      $(BUILD_DIR)/audio_player.o $(BUILD_DIR)/gpio.o
GUN_FX_DEMO_OBJS = $(BUILD_DIR)/demo/gun_fx_demo.o $(BUILD_DIR)/gun_fx.o \
                   $(BUILD_DIR)/lights.o $(BUILD_DIR)/smoke_generator.o \
                   $(BUILD_DIR)/audio_player.o $(BUILD_DIR)/gpio.o $(BUILD_DIR)/servo.o
SERVO_DEMO_OBJS = $(BUILD_DIR)/demo/servo_demo.o $(BUILD_DIR)/servo.o
JETIEX_DEMO_OBJS = $(BUILD_DIR)/demo/jetiex_demo.o $(BUILD_DIR)/jetiex.o

# Default target
.PHONY: all
all: $(TARGETS)

# Build without JetiEX telemetry support
# Usage: make ENABLE_JETIEX=0
.PHONY: nojetiex
nojetiex:
	$(MAKE) ENABLE_JETIEX=0

# Demo target
.PHONY: demo
demo: $(DEMO_TARGETS)

# Debug target - builds with debug logging enabled
.PHONY: debug
debug: CFLAGS += -DDEBUG
debug: clean all

# Create build directories
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)/demo

# Main application
$(HELIFX): $(BUILD_DIR) $(HELIFX_OBJS)
	$(CC) $(CFLAGS) -o $@ $(HELIFX_OBJS) $(LIBS)

# Demo programs
$(BUILD_DIR)/mixer_demo: $(BUILD_DIR) $(MIXER_DEMO_OBJS)
	$(CC) $(CFLAGS) -o $@ $(MIXER_DEMO_OBJS) $(LIBS)

$(BUILD_DIR)/engine_fx_demo: $(BUILD_DIR) $(ENGINE_FX_DEMO_OBJS)
	$(CC) $(CFLAGS) -o $@ $(ENGINE_FX_DEMO_OBJS) $(LIBS)

$(BUILD_DIR)/gun_fx_demo: $(BUILD_DIR) $(GUN_FX_DEMO_OBJS)
	$(CC) $(CFLAGS) -o $@ $(GUN_FX_DEMO_OBJS) $(LIBS)

$(BUILD_DIR)/servo_demo: $(BUILD_DIR) $(SERVO_DEMO_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SERVO_DEMO_OBJS) $(LIBS)

$(BUILD_DIR)/jetiex_demo: $(BUILD_DIR) $(JETIEX_DEMO_OBJS)
	$(CC) $(CFLAGS) -o $@ $(JETIEX_DEMO_OBJS) $(LIBS)

# Compile source files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Compile demo files
$(BUILD_DIR)/demo/%.o: $(DEMO_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Dependencies
$(BUILD_DIR)/main.o: $(INCLUDE_DIR)/engine_fx.h $(INCLUDE_DIR)/gun_fx.h \
                     $(INCLUDE_DIR)/audio_player.h $(INCLUDE_DIR)/gpio.h \
                     $(INCLUDE_DIR)/config_loader.h

$(BUILD_DIR)/config_loader.o: $(INCLUDE_DIR)/config_loader.h

$(BUILD_DIR)/engine_fx.o: $(INCLUDE_DIR)/engine_fx.h $(INCLUDE_DIR)/audio_player.h \
                          $(INCLUDE_DIR)/gpio.h

$(BUILD_DIR)/gun_fx.o: $(INCLUDE_DIR)/gun_fx.h $(INCLUDE_DIR)/lights.h \
                       $(INCLUDE_DIR)/smoke_generator.h $(INCLUDE_DIR)/audio_player.h \
                       $(INCLUDE_DIR)/gpio.h

$(BUILD_DIR)/audio_player.o: $(INCLUDE_DIR)/audio_player.h $(INCLUDE_DIR)/miniaudio.h

$(BUILD_DIR)/gpio.o: $(INCLUDE_DIR)/gpio.h

$(BUILD_DIR)/lights.o: $(INCLUDE_DIR)/lights.h $(INCLUDE_DIR)/gpio.h

$(BUILD_DIR)/smoke_generator.o: $(INCLUDE_DIR)/smoke_generator.h $(INCLUDE_DIR)/gpio.h

$(BUILD_DIR)/servo.o: $(INCLUDE_DIR)/servo.h

$(BUILD_DIR)/jetiex.o: $(INCLUDE_DIR)/jetiex.h

# Clean build artifacts
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)

# Install targets
.PHONY: install
install: all
	@echo "Installing helifx to /usr/local/bin/"
	sudo install -m 755 $(HELIFX) /usr/local/bin/
	@echo "Installing demo programs..."
	sudo install -m 755 $(BUILD_DIR)/mixer_demo /usr/local/bin/
	sudo install -m 755 $(BUILD_DIR)/engine_fx_demo /usr/local/bin/
	sudo install -m 755 $(BUILD_DIR)/gun_fx_demo /usr/local/bin/
	sudo install -m 755 $(BUILD_DIR)/servo_demo /usr/local/bin/
	sudo install -m 755 $(BUILD_DIR)/jetiex_demo /usr/local/bin/
	@echo "Installation complete"

# Install systemd service
.PHONY: install-service
install-service:
	@echo "Installing systemd service..."
	sudo install -m 644 $(SCRIPTS_DIR)/helifx.service /etc/systemd/system/
	sudo systemctl daemon-reload
	@echo "Service installed. Enable with: sudo systemctl enable helifx"

# Uninstall
.PHONY: uninstall
uninstall:
	@echo "Removing helifx binaries..."
	sudo rm -f /usr/local/bin/helifx
	sudo rm -f /usr/local/bin/mixer_demo
	sudo rm -f /usr/local/bin/engine_fx_demo
	sudo rm -f /usr/local/bin/gun_fx_demo
	sudo rm -f /usr/local/bin/servo_demo
	sudo rm -f /usr/local/bin/jetiex_demo
	@echo "Uninstallation complete"

# Help target
.PHONY: help
help:
	@echo "Helicopter FX Build System"
	@echo ""
	@echo "Prerequisites:"
	@echo "  - GCC 14+ (C23 support required)"
	@echo "  - pigpio library and daemon (GPIO control)"
	@echo "  - libcyaml, libyaml (configuration)"
	@echo "  - ALSA development libraries (audio)"
	@echo ""
	@echo "Install dependencies:"
	@echo "  sudo apt-get install build-essential libyaml-dev libasound2-dev pigpio"
	@echo ""
	@echo "Configure pigpio for WM8960 Audio HAT:"
	@echo "  sudo pigpiod -x 0x3C000C"
	@echo "  (See docs/PIGPIO_SETUP.md for permanent configuration)"
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build all targets (default)"
	@echo "  demo             - Build demo programs only"
	@echo "  debug            - Build with debug logging enabled (-DDEBUG)"
	@echo "  clean            - Remove build artifacts"
	@echo "  install          - Install binaries to /usr/local/bin"
	@echo "  install-service  - Install systemd service"
	@echo "  uninstall        - Remove installed binaries"
	@echo "  help             - Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make             - Build everything"
	@echo "  make demo        - Build only demos"
	@echo "  make debug       - Debug build (with LOG_DEBUG output)"
	@echo "  make clean all   - Clean rebuild"
	@echo "  sudo make install - Install to system"
