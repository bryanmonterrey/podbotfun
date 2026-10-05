CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wshadow -Wconversion -pedantic
# Host always compiles the world-view carousel in (device default is off).
CXXFLAGS += -DCONFIG_LILGUY_WORLD_VIEW=1
# Kconfig defaults this on; the host has no sdkconfig.h to read it from.
CXXFLAGS += -DCONFIG_LILGUY_SHOW_SELECTION_OVERLAY=1
INCLUDES := -Imain/include
CORE_SRCS := main/catalog.cpp main/eye_engine.cpp main/raster.cpp main/eye_renderer.cpp \
             main/capsule_face.cpp main/device_ui.cpp main/onboarding.cpp

# Desktop simulator: SDL2 window when sdl2-config is on PATH, otherwise a
# headless-only build that still renders frames to PPM.
SDL2_CONFIG := $(shell command -v sdl2-config 2>/dev/null)
SIM_PLATFORM_SRCS :=
ifeq ($(SDL2_CONFIG),)
SIM_FLAGS := -DEYES_SIM_HEADLESS_ONLY
SIM_LIBS :=
else
SIM_FLAGS := $(shell $(SDL2_CONFIG) --cflags)
SIM_LIBS := $(shell $(SDL2_CONFIG) --libs)
# The transparent, click-through window needs a little Cocoa that SDL2 does
# not wrap. Other platforms fall back to an ordinary opaque window.
ifeq ($(shell uname -s),Darwin)
SIM_FLAGS += -DEYES_SIM_MAC=1
SIM_PLATFORM_SRCS += host/mac_window.mm host/mac_camera.mm host/mac_speech.mm
SIM_LIBS += -framework Cocoa -framework AVFoundation -framework CoreMedia -framework CoreVideo \
            -framework Vision -framework CoreImage -framework Speech
# The webcam-backed CameraService needs an NSCameraUsageDescription; a bare
# CLI binary carries it in an embedded Info.plist section.
SIM_LIBS += -Wl,-sectcreate,__TEXT,__info_plist,host/sim_info.plist
endif
endif

.PHONY: host-test host-auth-test host-preview host-sim host-sim-selftest earcon-preview clean

# The test binary also writes /tmp/capsule_grid.ppm (5x5 of the 25 poses) and
# /tmp/capsule_blink.ppm (blink envelope strip); convert to png when possible.
host-test: host/eyes_host_tests
	./host/eyes_host_tests
	@for image in capsule_grid capsule_blink; do \
		if command -v magick >/dev/null 2>&1; then \
			magick /tmp/$$image.ppm /tmp/$$image.png; \
		elif command -v convert >/dev/null 2>&1; then \
			convert /tmp/$$image.ppm /tmp/$$image.png; \
		elif command -v sips >/dev/null 2>&1; then \
			sips -s format png /tmp/$$image.ppm --out /tmp/$$image.png >/dev/null; \
		fi; \
	done

host-preview: host/eyes_preview
	./host/eyes_preview host/eyes_preview.ppm
	@if command -v magick >/dev/null 2>&1; then \
		magick host/eyes_preview.ppm host/eyes_preview.png; \
	elif command -v convert >/dev/null 2>&1; then \
		convert host/eyes_preview.ppm host/eyes_preview.png; \
	fi

host/eyes_host_tests: host/test_main.cpp $(CORE_SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# On-device passkey/biometric fakes. Needs OpenSSL (P-256) on the host only — the
# device uses mbedTLS. Isolated to this target so nothing else links OpenSSL.
OPENSSL_PREFIX := $(shell brew --prefix openssl@3 2>/dev/null || echo /opt/homebrew/opt/openssl@3)
host-auth-test: host/eyes_auth_tests
	./host/eyes_auth_tests
host/eyes_auth_tests: host/test_auth.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -I$(OPENSSL_PREFIX)/include $^ -L$(OPENSSL_PREFIX)/lib -lcrypto -o $@

host/eyes_preview: host/preview.cpp $(CORE_SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# Audible earcon preview: renders each recipe (four varied plays) to host/earcon_*.wav.
earcon-preview: host/earcon_preview
	./host/earcon_preview
host/earcon_preview: host/earcon_preview.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

host-sim: host/eyes_simulator
	./host/eyes_simulator

# Drives every panel control and checks it reaches the firmware; needs no display.
host-sim-selftest: host/eyes_simulator
	./host/eyes_simulator --selftest

host/eyes_simulator: host/simulator.cpp $(SIM_PLATFORM_SRCS) $(CORE_SRCS)
	$(CXX) $(CXXFLAGS) $(SIM_FLAGS) $(INCLUDES) $^ $(SIM_LIBS) -o $@

# Native camera helpers for Mac computer-use: gaze context (coarse head pose)
# and the gesture trigger (blink/wink → click/confirm). macOS-only; mac-mcp
# shells out to them. `make gaze-helper` builds both.
gaze-helper: tools/gaze/gaze_helper tools/gaze/gesture_helper tools/gaze/snap_helper tools/gaze/trigger_daemon
tools/gaze/gaze_helper: tools/gaze/gaze_helper.swift
	swiftc -O $< -o $@ -framework AVFoundation -framework Vision
tools/gaze/gesture_helper: tools/gaze/gesture_helper.swift
	swiftc -O $< -o $@ -framework AVFoundation -framework CoreImage
tools/gaze/snap_helper: tools/gaze/snap_helper.swift
	swiftc -O $< -o $@ -framework AVFoundation
tools/gaze/trigger_daemon: tools/gaze/trigger_daemon.swift
	swiftc -O $< -o $@ -framework AVFoundation -framework CoreImage

clean:
	rm -f host/eyes_host_tests host/eyes_preview host/eyes_simulator host/*.ppm \
	      host/earcon_preview host/earcon_*.wav \
	      tools/gaze/gaze_helper tools/gaze/gesture_helper tools/gaze/snap_helper tools/gaze/trigger_daemon \
	      host/eyes_preview.png
