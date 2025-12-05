# Compiler settings
CC = gcc
CFLAGS = -Wall -Wextra -Werror -Wno-unused-function -std=c11 -pthread -O2 -D_DEFAULT_SOURCE
INCLUDES = -I./include
LIBS = -lpthread -ldl -lm -latomic -lyaml

# Directories
SRC_DIR = src
INCLUDE_DIR = include
BUILD_DIR = build
DEMO_DIR = demo
SCRIPTS_DIR = scripts

# Output binaries
HELIFX = $(BUILD_DIR)/helifx
DEMO_TARGETS = $(BUILD_DIR)/mixer_demo $(BUILD_DIR)/gpio_demo \
               $(BUILD_DIR)/engine_fx_demo $(BUILD_DIR)/gun_fx_demo \
               $(BUILD_DIR)/servo_demo

# All targets
TARGETS = $(HELIFX) $(DEMO_TARGETS)

# Source files
HELIFX_SRCS = $(SRC_DIR)/main.c $(SRC_DIR)/config_loader.c \
              $(SRC_DIR)/engine_fx.c $(SRC_DIR)/gun_fx.c \
              $(SRC_DIR)/lights.c $(SRC_DIR)/smoke_generator.c \
              $(SRC_DIR)/audio_player.c $(SRC_DIR)/gpio.c $(SRC_DIR)/servo.c

MIXER_DEMO_SRCS = $(DEMO_DIR)/mixer_demo.c $(SRC_DIR)/audio_player.c $(SRC_DIR)/gpio.c
GPIO_DEMO_SRCS = $(DEMO_DIR)/gpio_demo.c $(SRC_DIR)/gpio.c
ENGINE_FX_DEMO_SRCS = $(DEMO_DIR)/engine_fx_demo.c $(SRC_DIR)/engine_fx.c \
                      $(SRC_DIR)/audio_player.c $(SRC_DIR)/gpio.c
GUN_FX_DEMO_SRCS = $(DEMO_DIR)/gun_fx_demo.c $(SRC_DIR)/gun_fx.c \
                   $(SRC_DIR)/lights.c $(SRC_DIR)/smoke_generator.c \
                   $(SRC_DIR)/audio_player.c $(SRC_DIR)/gpio.c $(SRC_DIR)/servo.c
SERVO_DEMO_SRCS = $(DEMO_DIR)/servo_demo.c $(SRC_DIR)/servo.c

# Object files
HELIFX_OBJS = $(HELIFX_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
MIXER_DEMO_OBJS = $(BUILD_DIR)/demo/mixer_demo.o $(BUILD_DIR)/audio_player.o $(BUILD_DIR)/gpio.o
GPIO_DEMO_OBJS = $(BUILD_DIR)/demo/gpio_demo.o $(BUILD_DIR)/gpio.o
ENGINE_FX_DEMO_OBJS = $(BUILD_DIR)/demo/engine_fx_demo.o $(BUILD_DIR)/engine_fx.o \
                      $(BUILD_DIR)/audio_player.o $(BUILD_DIR)/gpio.o
GUN_FX_DEMO_OBJS = $(BUILD_DIR)/demo/gun_fx_demo.o $(BUILD_DIR)/gun_fx.o \
                   $(BUILD_DIR)/lights.o $(BUILD_DIR)/smoke_generator.o \
                   $(BUILD_DIR)/audio_player.o $(BUILD_DIR)/gpio.o $(BUILD_DIR)/servo.o
SERVO_DEMO_OBJS = $(BUILD_DIR)/demo/servo_demo.o $(BUILD_DIR)/servo.o

# Default target
.PHONY: all
all: $(TARGETS)

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

$(BUILD_DIR)/gpio_demo: $(BUILD_DIR) $(GPIO_DEMO_OBJS)
	$(CC) $(CFLAGS) -o $@ $(GPIO_DEMO_OBJS) $(LIBS)

$(BUILD_DIR)/engine_fx_demo: $(BUILD_DIR) $(ENGINE_FX_DEMO_OBJS)
	$(CC) $(CFLAGS) -o $@ $(ENGINE_FX_DEMO_OBJS) $(LIBS)

$(BUILD_DIR)/gun_fx_demo: $(BUILD_DIR) $(GUN_FX_DEMO_OBJS)
	$(CC) $(CFLAGS) -o $@ $(GUN_FX_DEMO_OBJS) $(LIBS)

$(BUILD_DIR)/servo_demo: $(BUILD_DIR) $(SERVO_DEMO_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SERVO_DEMO_OBJS) $(LIBS)

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
	sudo install -m 755 $(BUILD_DIR)/gpio_demo /usr/local/bin/
	sudo install -m 755 $(BUILD_DIR)/engine_fx_demo /usr/local/bin/
	sudo install -m 755 $(BUILD_DIR)/gun_fx_demo /usr/local/bin/
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
	sudo rm -f /usr/local/bin/gpio_demo
	sudo rm -f /usr/local/bin/engine_fx_demo
	sudo rm -f /usr/local/bin/gun_fx_demo
	@echo "Uninstallation complete"

# Help target
.PHONY: help
help:
	@echo "Helicopter FX Build System"
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
