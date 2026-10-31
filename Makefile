# Cross-compiles the Windows x64 ASI from Linux with mingw-w64 (static CRT: no redistributable needed).
CXX      := x86_64-w64-mingw32-g++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wpedantic
LDFLAGS  := -shared -static -s
LDLIBS   := -lversion
SRC      := $(wildcard src/*.cpp)
OUT      := build/GTAV.FirstPersonExtended.asi

$(OUT): $(SRC) $(wildcard src/*.h)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SRC) -o $@ $(LDFLAGS) $(LDLIBS)

# Diagnostic build that logs which camera FOV code runs (never shipped).
probe: $(SRC) $(wildcard src/*.h)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DFPFIX_CAMERA_PROBE $(SRC) -o build/GTAV.FirstPersonExtended_probe.asi $(LDFLAGS) $(LDLIBS)

# Diagnostic build that counts run/sprint constant loads per held key combo (never shipped).
sprintprobe: $(SRC) $(wildcard src/*.h)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DFPFIX_SPRINT_PROBE $(SRC) -o build/GTAV.FirstPersonExtended_sprintprobe.asi $(LDFLAGS) $(LDLIBS)

# Diagnostic build that logs the first-person sprint gate's value/threshold per key combo (never shipped).
gateprobe: $(SRC) $(wildcard src/*.h)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DFPFIX_SPRINT_GATE_PROBE $(SRC) -o build/GTAV.FirstPersonExtended_gateprobe.asi $(LDFLAGS) $(LDLIBS)

# Diagnostic build that counts the footstep-audio functions per key combo (never shipped).
footstepprobe: $(SRC) $(wildcard src/*.h)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DFPFIX_FOOTSTEP_PROBE $(SRC) -o build/GTAV.FirstPersonExtended_footstepprobe.asi $(LDFLAGS) $(LDLIBS)

# Test build with the ADS signatures corrupted on purpose (proves safe failure; never shipped).
failtest: $(SRC) $(wildcard src/*.h)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DFPFIX_SIMULATE_SCAN_FAILURE $(SRC) -o build/GTAV.FirstPersonExtended_failtest.asi $(LDFLAGS) $(LDLIBS)

# Development tool: writes the decrypted in-memory game image for offline analysis.
dumper: tools/image_dump/ImageDump.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o build/GTAV.FirstPersonExtended_ImageDump.asi $(LDFLAGS)

clean:
	rm -rf build

.PHONY: clean probe sprintprobe gateprobe footstepprobe failtest dumper
