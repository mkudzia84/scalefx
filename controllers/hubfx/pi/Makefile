# HubFX Pi Build System
# Parallel build disabled (set -j manually if desired)
# MAKEFLAGS = -j 4

# Compiler settings
CC = gcc
CFLAGS = -Wall -Wextra -Werror -Wno-unused-function -std=c23 -D_DEFAULT_SOURCE
INCLUDES = -I./include
# Dependencies:
# - libcyaml (YAML parsing) depends on libyaml
# - libgpiod (GPIO control) - modern Linux GPIO interface
# - Audio: ALSA libs (libasound2)
# - libatomic (atomic operations for miniaudio)
LIBS = -lm -lcyaml -lyaml -lgpiod -lpthread -latomic

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
SCRIPTS_DIR = scripts

# Output binaries
HUBFX = $(BUILD_DIR)/hubfx

# All targets
TARGETS = $(HUBFX)

# Source files
HUBFX_SRCS = $(SRC_DIR)/main.c $(SRC_DIR)/config_loader.c \
              $(SRC_DIR)/engine_fx.c $(SRC_DIR)/gun_fx.c \
              $(SRC_DIR)/smoke_generator.c \
              $(SRC_DIR)/audio_player.c $(SRC_DIR)/gpio.c $(SRC_DIR)/serial_bus.c \
              $(SRC_DIR)/status.c $(SRC_DIR)/logging.c

# Object files
HUBFX_OBJS = $(HUBFX_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# Default target
.PHONY: all
all: $(TARGETS)

# Debug target - builds with debug logging enabled
.PHONY: debug
debug: CFLAGS += -DDEBUG
debug: clean all

# Create build directories
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Main application
$(HUBFX): $(BUILD_DIR) $(HUBFX_OBJS)
	$(CC) $(CFLAGS) -o $@ $(HUBFX_OBJS) $(LIBS)

# Compile source files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Dependencies
$(BUILD_DIR)/main.o: $(INCLUDE_DIR)/engine_fx.h $(INCLUDE_DIR)/gun_fx.h \
                     $(INCLUDE_DIR)/audio_player.h $(INCLUDE_DIR)/gpio.h \
                     $(INCLUDE_DIR)/config_loader.h

$(BUILD_DIR)/config_loader.o: $(INCLUDE_DIR)/config_loader.h

$(BUILD_DIR)/engine_fx.o: $(INCLUDE_DIR)/engine_fx.h $(INCLUDE_DIR)/audio_player.h \
                          $(INCLUDE_DIR)/gpio.h

$(BUILD_DIR)/gun_fx.o: $(INCLUDE_DIR)/gun_fx.h \
                       $(INCLUDE_DIR)/smoke_generator.h $(INCLUDE_DIR)/audio_player.h \
                       $(INCLUDE_DIR)/gpio.h

$(BUILD_DIR)/audio_player.o: $(INCLUDE_DIR)/audio_player.h $(INCLUDE_DIR)/miniaudio.h

$(BUILD_DIR)/gpio.o: $(INCLUDE_DIR)/gpio.h

$(BUILD_DIR)/smoke_generator.o: $(INCLUDE_DIR)/smoke_generator.h $(INCLUDE_DIR)/gpio.h

$(BUILD_DIR)/serial_bus.o: $(INCLUDE_DIR)/serial_bus.h

# Clean build artifacts
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)

# Install targets
.PHONY: install
install: all
	@echo "Installing hubfx to /usr/local/bin/"
	sudo install -m 755 $(HUBFX) /usr/local/bin/
	@echo "Installation complete"

# Install systemd service
.PHONY: install-service
install-service:
	@echo "Installing systemd service..."
	sudo install -m 644 $(SCRIPTS_DIR)/hubfx.service /etc/systemd/system/
	sudo systemctl daemon-reload
	@echo "Service installed. Enable with: sudo systemctl enable hubfx"

# Uninstall
.PHONY: uninstall
uninstall:
	@echo "Removing hubfx binaries..."
	sudo rm -f /usr/local/bin/hubfx
	@echo "Uninstallation complete"

# Help target
.PHONY: help
help:
	@echo "HubFX Pi Build System"
	@echo ""
	@echo "Prerequisites:"
	@echo "  - GCC 14+ (C23 support required)"
	@echo "  - libgpiod (GPIO control - modern kernel interface)"
	@echo "  - libcyaml, libyaml (configuration)"
	@echo "  - ALSA development libraries (audio)"
	@echo ""
	@echo "Install dependencies:"
	@echo "  sudo apt-get install build-essential libyaml-dev libcyaml-dev libasound2-dev libgpiod-dev gpiod"
	@echo ""
	@echo "User Permissions:"
	@echo "  Add user to gpio group: sudo usermod -a -G gpio \$$USER"
	@echo "  Then log out and back in for group membership to take effect"
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build hubfx (default)"
	@echo "  debug            - Build with debug logging enabled (-DDEBUG)"
	@echo "  clean            - Remove build artifacts"
	@echo "  install          - Install binary to /usr/local/bin"
	@echo "  install-service  - Install systemd service"
	@echo "  uninstall        - Remove installed binary"
	@echo "  help             - Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make             - Build hubfx"
	@echo "  make debug       - Debug build (with LOG_DEBUG output)"
	@echo "  make clean all   - Clean rebuild"
	@echo "  sudo make install - Install to system"
