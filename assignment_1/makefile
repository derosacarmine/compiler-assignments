# Nome del plugin generato da CMake (cambialo se diverso)
PLUGIN_NAME=libLocalOpts.so
BUILD_DIR=build
TEST_DIR=tests
# Percorso del plugin dopo la compilazione
PLUGIN_PATH=$(BUILD_DIR)/LocalOpts/$(PLUGIN_NAME)

.PHONY: all clean setup optimize

all: setup

setup:
	@echo "Select LLVM installation path:"
	@echo "1) /usr/lib/llvm-19/bin"
	@echo "2) Insert manually"
	@read -p "Choose option (1-2): " opt; \
	if [ "$$opt" = "1" ]; then \
		export LLVM_DIR=/usr/lib/llvm-19/bin; \
	else \
		read -p "Enter path: " manual_path; \
		export LLVM_DIR=$$manual_path; \
	fi; \
	chmod +x init.sh; \
	./init.sh $$LLVM_DIR

optimize:
	@if [ ! -f $(PLUGIN_PATH) ]; then \
		echo "Error: Plugin not found. Run 'make setup' first."; \
		exit 1; \
	fi
	@chmod +x optimize_tests.sh
	@read -p "Enter passes (e.g., algebraic-identity strength-reduction): " passes; \
	./optimize_tests.sh -t $(TEST_DIR) -p $(PLUGIN_PATH) $$passes

clean:
	rm -rf $(BUILD_DIR)