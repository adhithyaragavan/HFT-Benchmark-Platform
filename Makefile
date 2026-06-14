.PHONY: help build test test-integration up down clean lint

SHELL := /bin/bash
PROJECT_ROOT := $(shell pwd)
SERVICES := gateway submission-svc sandbox-mgr bot-orchestrator bot-worker scoring-engine leaderboard-svc cpp-exchange

# Auto-detect vcpkg toolchain file location
VCPKG_TOOLCHAIN ?= $(shell \
	if [ -f "$(HOME)/vcpkg/scripts/buildsystems/vcpkg.cmake" ]; then \
		echo "$(HOME)/vcpkg/scripts/buildsystems/vcpkg.cmake"; \
	elif [ -f "/usr/local/share/vcpkg/scripts/buildsystems/vcpkg.cmake" ]; then \
		echo "/usr/local/share/vcpkg/scripts/buildsystems/vcpkg.cmake"; \
	elif [ -f "/opt/vcpkg/scripts/buildsystems/vcpkg.cmake" ]; then \
		echo "/opt/vcpkg/scripts/buildsystems/vcpkg.cmake"; \
	else \
		echo "vcpkg.cmake-NOTFOUND"; \
	fi \
)

help: ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | sort | awk 'BEGIN {FS = ":.*?## "}; {printf "\033[36m%-20s\033[0m %s\n", $$1, $$2}'

# ─── Build ────────────────────────────────────────────────────────────
build: ## Build all services and tools using CMake
	@if [ "$(VCPKG_TOOLCHAIN)" = "vcpkg.cmake-NOTFOUND" ]; then \
		echo -e "\033[31mError: vcpkg.cmake not found. Please set VCPKG_TOOLCHAIN env var.\033[0m"; \
		exit 1; \
	fi
	cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$(VCPKG_TOOLCHAIN) -DCMAKE_BUILD_TYPE=Release -DVCPKG_MANIFEST_DIR=$(PROJECT_ROOT)/services
	cmake --build build --parallel $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
	@echo "All C++ services built successfully in build/ directory."

build-%: ## Build a specific service/target (e.g., make build-gateway)
	@if [ "$(VCPKG_TOOLCHAIN)" = "vcpkg.cmake-NOTFOUND" ]; then \
		echo -e "\033[31mError: vcpkg.cmake not found. Please set VCPKG_TOOLCHAIN env var.\033[0m"; \
		exit 1; \
	fi
	cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$(VCPKG_TOOLCHAIN) -DCMAKE_BUILD_TYPE=Release -DVCPKG_MANIFEST_DIR=$(PROJECT_ROOT)/services
	cmake --build build --target $*

# ─── Test ─────────────────────────────────────────────────────────────
test: ## Run unit tests using ctest
	@if [ -d "build" ]; then \
		cd build && ctest --output-on-failure; \
	else \
		echo "Please build the project first using 'make build'."; \
	fi

test-%: ## Test a specific C++ target
	cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$(VCPKG_TOOLCHAIN) -DCMAKE_BUILD_TYPE=Debug -DVCPKG_MANIFEST_DIR=$(PROJECT_ROOT)/services
	cmake --build build --target $*
	cd build && ./pkg/orderbook/$*  # Example for orderbook tests if any

test-integration: ## Run integration tests with full stack
	docker compose -f deploy/docker-compose.yml -f deploy/docker-compose.test.yml up --build --abort-on-container-exit --exit-code-from integration-tests

# ─── Docker compose (Infrastructure) ──────────────────────────────────
up: ## Start local infrastructure (DBs, Cache, Kafka, S3)
	docker compose -f deploy/docker-compose.yml up -d
	@echo "Waiting for infrastructure to be healthy..."
	@sleep 8
	@echo "Local infrastructure environment is ready!"
	@echo "  PostgreSQL:   localhost:5432 (benchmarks)"
	@echo "  TimescaleDB:  localhost:5433 (telemetry)"
	@echo "  Redis:        localhost:6379 (leaderboard)"
	@echo "  Redpanda:     localhost:19092"
	@echo "  MinIO:        localhost:9000 (console: localhost:9001)"

down: ## Stop local infrastructure
	docker compose -f deploy/docker-compose.yml down

clean: ## Stop everything and remove build outputs / docker volumes
	docker compose -f deploy/docker-compose.yml down -v --remove-orphans
	rm -rf build/ bin/

# ─── Dev mode ─────────────────────────────────────────────────────────
dev-%: ## Compile and run a specific service locally (e.g., make dev-gateway)
	cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$(VCPKG_TOOLCHAIN) -DCMAKE_BUILD_TYPE=Release -DVCPKG_MANIFEST_DIR=$(PROJECT_ROOT)/services
	cmake --build build --target $*
	@echo "Running $*..."
	@if [ "$*" = "cpp-exchange" ]; then \
		./build/templates/cpp-exchange/cpp-exchange; \
	else \
		./build/services/$*/$*; \
	fi

# ─── Demo ─────────────────────────────────────────────────────────────
demo: ## Run the end-to-end demo script
	./scripts/run-demo.sh

# ─── Lint ─────────────────────────────────────────────────────────────
lint: ## Check formatting using clang-format (if installed)
	@if command -v clang-format >/dev/null 2>&1; then \
		echo "Running clang-format check..."; \
		find services pkg templates -name "*.cpp" -o -name "*.hpp" -o -name "*.h" | xargs clang-format --dry-run -Werror && echo "Linter passed!" || exit 1; \
	else \
		echo "clang-format is not installed. Skipping formatting check."; \
	fi
