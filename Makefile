BUILD ?= build
JOBS ?= $(shell n=$$(sysctl -n hw.ncpu 2>/dev/null || nproc); echo $$(( n / 2 > 2 ? n / 2 : 2 )))
LLVM ?= $(shell brew --prefix llvm 2>/dev/null)

.PHONY: build test asan tsan bench fuzz lint demo clean

build:
	cmake -S . -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j$(JOBS)

test: build
	ctest --test-dir $(BUILD) --output-on-failure -j$(JOBS)

asan:
	cmake -S . -B build-asan -G Ninja -DHEARTH_SANITIZE=address -DCMAKE_BUILD_TYPE=RelWithDebInfo
	cmake --build build-asan -j$(JOBS)
	ctest --test-dir build-asan --output-on-failure -j$(JOBS)

tsan:
	cmake -S . -B build-tsan -G Ninja -DHEARTH_SANITIZE=thread -DCMAKE_BUILD_TYPE=RelWithDebInfo
	cmake --build build-tsan -j$(JOBS)
	ctest --test-dir build-tsan --output-on-failure -j$(JOBS)

# Needs nginx on PATH. Runs alone: it saturates 4 server threads plus the load generator.
bench: build
	bench/scripts/run.sh $(BUILD) bench/results/raw.jsonl
	python3 bench/scripts/summarize.py bench/results/raw.jsonl > bench/results/summary.md

FUZZTIME ?= 60
CXX_FUZZ ?= $(if $(LLVM),$(LLVM)/bin/clang++,clang++)
fuzz:
	mkdir -p $(BUILD)
	$(CXX_FUZZ) -std=c++20 -O1 -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
	  -Iinclude src/http.cpp fuzz/fuzz_parse.cpp -o $(BUILD)/hearth_fuzz
	mkdir -p fuzz/corpus && $(BUILD)/hearth_fuzz -max_total_time=$(FUZZTIME) -max_len=4096 fuzz/corpus

lint:
	clang-format --dry-run -Werror src/*.cpp include/hearth/*.h tests/*.cpp tools/*.cpp fuzz/*.cpp

demo:
	@$(MAKE) -s build > /dev/null
	@bench/scripts/demo.sh $(BUILD)

clean:
	rm -rf build build-asan build-tsan
