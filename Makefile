CXX ?= g++
CXXFLAGS := -std=c++17 -Wall -Wextra -pedantic -I aprs_hunter
FIRMWARE_SRC := aprs_hunter/aprs_frame.cpp aprs_hunter/records.cpp aprs_hunter/display_logic.cpp aprs_hunter/stations.cpp aprs_hunter/double_reset.cpp
TEST_SRC := $(wildcard tests/*.cpp)
VENV := .venv

.PHONY: test test-firmware test-tools venv clean

test: test-firmware test-tools

test-firmware: build/host_tests
	./build/host_tests

build/host_tests: $(TEST_SRC) $(FIRMWARE_SRC) $(wildcard aprs_hunter/*.h)
	mkdir -p build
	$(CXX) $(CXXFLAGS) $(TEST_SRC) $(FIRMWARE_SRC) -o $@

test-tools: $(VENV)/bin/python
	$(VENV)/bin/python -m unittest discover -s tools -v

venv: $(VENV)/bin/python

$(VENV)/bin/python: tools/requirements.txt
	python3 -m venv $(VENV)
	$(VENV)/bin/pip install --quiet -r tools/requirements.txt
	touch $@

clean:
	rm -rf build
