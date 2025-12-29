

# Compiler settings
CC = gcc
AS = nasm
CFLAGS = -std=c99 -Wall -Wextra -O2 -g -fPIC -Ilib
ASFLAGS = -f elf64
LDFLAGS = -lpthread -lm -lncurses

# Directories
SRC_DIR = lib
ASM_DIR = asm
APP_DIR = app
BIN_DIR = bin
TEST_DIR = tests
OBJ_DIR = obj

# Source files
LIB_SOURCES = $(wildcard $(SRC_DIR)/*.c)
ASM_SOURCES = $(wildcard $(ASM_DIR)/*.asm)
APP_SOURCES = $(wildcard $(APP_DIR)/*.c)
TEST_SOURCES = $(wildcard $(TEST_DIR)/*.c)

# Object files
LIB_OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(LIB_SOURCES))
ASM_OBJECTS = $(patsubst $(ASM_DIR)/%.asm,$(OBJ_DIR)/%.o,$(ASM_SOURCES))
APP_OBJECTS = $(patsubst $(APP_DIR)/%.c,$(OBJ_DIR)/%.o,$(APP_SOURCES))
TEST_OBJECTS = $(patsubst $(TEST_DIR)/%.c,$(OBJ_DIR)/%.o,$(TEST_SOURCES))

# Targets
LIBRARY = $(BIN_DIR)/libnetcomm.a
SERVER = $(BIN_DIR)/server
CLIENT = $(BIN_DIR)/client
TESTS = $(BIN_DIR)/tests
BENCHMARK = $(BIN_DIR)/benchmark

# Default target
all: clean lib app

# Create directories
dirs:
	@mkdir -p $(OBJ_DIR) $(BIN_DIR)

# Build library
lib: dirs $(LIBRARY)

$(LIBRARY): $(LIB_OBJECTS) $(ASM_OBJECTS)
	@echo "Building static library..."
	@ar rcs $@ $^
	@echo "Library built: $@"

# Build applications
app: dirs $(SERVER) $(CLIENT)

$(SERVER): $(OBJ_DIR)/server.o $(LIBRARY)
	@echo "Building server..."
	@$(CC) $(CFLAGS) -no-pie -o $@ $^ $(LDFLAGS)
	@echo "Server built: $@"

$(CLIENT): $(OBJ_DIR)/client.o $(LIBRARY)
	@echo "Building client..."
	@$(CC) $(CFLAGS) -no-pie -o $@ $^ $(LDFLAGS)
	@echo "Client built: $@"

# Build tests
test: dirs $(TESTS)

$(TESTS): $(TEST_OBJECTS) $(LIBRARY)
	@echo "Building tests..."
	@$(CC) $(CFLAGS) -no-pie -o $@ $^ $(LDFLAGS)
	@echo "Tests built: $@"

# Build benchmarks
benchmark: dirs $(BENCHMARK)

$(BENCHMARK): $(TEST_OBJECTS) $(LIBRARY)
	@echo "Building benchmarks..."
	@$(CC) $(CFLAGS) -O3 -o $@ $^ $(LDFLAGS)
	@echo "Benchmarks built: $@"

# Object file rules
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "Compiling C source: $<"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/%.o: $(ASM_DIR)/%.asm
	@echo "Assembling: $<"
	@$(AS) $(ASFLAGS) $< -o $@

$(OBJ_DIR)/%.o: $(APP_DIR)/%.c
	@echo "Compiling app source: $<"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/%.o: $(TEST_DIR)/%.c
	@echo "Compiling test source: $<"
	@$(CC) $(CFLAGS) -c -o $@ $<

# Clean targets
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(OBJ_DIR) $(BIN_DIR)
	@echo "Clean complete"

clean-obj:
	@echo "Cleaning object files..."
	@rm -rf $(OBJ_DIR)
	@echo "Object files cleaned"

# Run targets
run-server: $(SERVER)
	@echo "Starting server..."
	@$(SERVER)

run-client: $(CLIENT)
	@echo "Starting client..."
	@$(CLIENT)

run-tests: $(TESTS)
	@echo "Running tests..."
	@$(TESTS)

run-benchmark: $(BENCHMARK)
	@echo "Running benchmarks..."
	@$(BENCHMARK)

# Install (copy binaries to system path)
install: all
	@echo "Installing to /usr/local/bin..."
	@sudo cp $(SERVER) /usr/local/bin/
	@sudo cp $(CLIENT) /usr/local/bin/
	@echo "Installation complete"

# Uninstall
uninstall:
	@echo "Removing from /usr/local/bin..."
	@sudo rm -f /usr/local/bin/server
	@sudo rm -f /usr/local/bin/client
	@echo "Uninstallation complete"

# Show help
help:
	@echo "Available targets:"
	@echo "  all           - Build everything (default)"
	@echo "  lib           - Build library only"
	@echo "  app           - Build applications only"
	@echo "  test          - Build and run tests"
	@echo "  benchmark     - Build and run benchmarks"
	@echo "  clean         - Remove all build artifacts"
	@echo "  run-server    - Build and run server"
	@echo "  run-client    - Build and run client"
	@echo "  install       - Install binaries to /usr/local/bin"
	@echo "  uninstall     - Remove installed binaries"
	@echo "  help          - Show this help"

# Phony targets
.PHONY: all lib app test benchmark clean clean-obj run-server run-client run-tests run-benchmark install uninstall help
