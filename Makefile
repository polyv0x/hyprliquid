CXX ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -fPIC -std=c++23
ifneq ($(filter g++ c++,$(notdir $(CXX))),)
CXXFLAGS += -fno-gnu-unique
endif
CPPFLAGS += $(shell pkg-config --cflags hyprland pixman-1 libdrm json-c)
LDLIBS += $(shell pkg-config --libs hyprland json-c)

TARGET = hyprliquid.so
OBJCOPY ?= objcopy
RELEASE_DIR ?= build/release
SOURCES = $(wildcard src/*.cpp)
OBJECTS = $(patsubst src/%.cpp,build/%.o,$(SOURCES))
CARGO ?= cargo
RUST_TARGET_DIR = $(CURDIR)/build/cargo
RUST_LIB = $(RUST_TARGET_DIR)/release/libhyprliquid_bridge.a
RUST_HEADERS = $(RUST_TARGET_DIR)/cxxbridge
RUST_HIDE = -Wl,--exclude-libs,libhyprliquid_bridge.a
RUST_INPUTS = $(wildcard crates/*/src/*.rs crates/*/src/*.in crates/*/Cargo.toml crates/*/build.rs) \
              Cargo.toml Cargo.lock rust-toolchain.toml .cargo/config.toml \
              src/MaterialParams.inc materials/liquid.json scripts/embed-rust.py
RUST_LINK = build/RustCore.o $(RUST_LIB) -ldl -lpthread -lm
RUST_FAULT_DIR = $(CURDIR)/build/cargo-faults
RUST_FAULT_LIB = $(RUST_FAULT_DIR)/release/libhyprliquid_bridge.a

all: $(TARGET)

# Keep normal development/hyprpm builds debuggable. Package only a private copy
# and publish the stripped plugin and matching symbols together by atomic rename.
release: $(TARGET) scripts/package-release.py
	python3 scripts/package-release.py "$(TARGET)" "$(RELEASE_DIR)" --objcopy "$(OBJCOPY)"

test-release: $(TARGET)
	python3 scripts/test-release.py

build:
	mkdir -p $@

build/%.o: src/%.cpp build/LiquidMaterialData.hpp | build
	$(CXX) $(CPPFLAGS) -Ibuild $(CXXFLAGS) -MMD -MP -c $< -o $@

build/ShaderManager.o: build/ShaderSources.hpp

# Cargo also tracks schema inputs. Forward the compiler and ABI flags used by
# the host adapter to CXX's generated C++ translation unit.
$(RUST_LIB): $(RUST_INPUTS) Makefile | build
	CXX="$(CXX)" CXXFLAGS="$(CXXFLAGS)" CARGO_TARGET_DIR="$(RUST_TARGET_DIR)" $(CARGO) build --release --locked -p hyprliquid-bridge

build/RustCore.o: $(RUST_LIB)
build/RustCore.o: CPPFLAGS += -I$(RUST_HEADERS)

build/ShaderSources.hpp: $(wildcard shaders/*.frag shaders/*.glsl) scripts/embed-shaders.py | build
	python3 scripts/embed-shaders.py $@

$(TARGET): $(OBJECTS) $(RUST_LIB) Makefile
# Keep the embedded Rust runtime and CXX helpers private to this plugin. Other
# in-process plugins may embed a different Rust/CXX runtime.
	$(CXX) -shared $(LDFLAGS) $(OBJECTS) $(RUST_LIB) $(RUST_HIDE) $(LDLIBS) -ldl -lpthread -lm -o build/$@.tmp
	mv build/$@.tmp $@

build/LiquidMaterialData.hpp: materials/liquid.json scripts/embed-material.py | build
	python3 scripts/embed-material.py $@

build/render-test: tests/render.cpp tests/liquid-render.hpp tests/liquid-exact.hpp tests/glass-exact.hpp src/GlassInterior.hpp src/LiquidUniforms.hpp src/Material.hpp src/LiquidMaterial.hpp build/LiquidMaterialData.hpp src/MaterialParams.inc src/BuiltInPresets.hpp src/PixelTransfer.hpp src/RustCore.hpp build/RustCore.o $(RUST_LIB) | build
	$(CXX) $(CXXFLAGS) -Isrc -Ibuild $< $(RUST_LINK) -o $@ $$(pkg-config --cflags --libs egl glesv2)

build/security-test: tests/security.cpp src/ResizeAngle.hpp src/PointerAngle.hpp src/RenderSafety.hpp src/Material.hpp src/LiquidMaterial.hpp src/BuiltInPresets.hpp build/LiquidMaterialData.hpp src/MaterialParams.inc src/LuaSafety.hpp src/RustCore.hpp build/RustCore.o $(RUST_LIB) | build
	$(CXX) $(CXXFLAGS) -UNDEBUG -Isrc -Ibuild $< $(RUST_LINK) -o $@ $$(pkg-config --cflags --libs lua)

build/rust-differential-test: tests/rust-differential.cpp tests/reference/material.hpp tests/reference/render-safety.hpp src/RustCore.hpp src/Material.hpp src/MaterialParams.inc build/LiquidMaterialData.hpp build/RustCore.o $(RUST_LIB) | build
	$(CXX) $(CXXFLAGS) -UNDEBUG -Isrc -Ibuild $< $(RUST_LINK) -o $@

# A separate archive keeps the deliberate panic entry point out of production.
$(RUST_FAULT_LIB): $(RUST_INPUTS) Makefile | build
	CXX="$(CXX)" CXXFLAGS="$(CXXFLAGS)" CARGO_TARGET_DIR="$(RUST_FAULT_DIR)" $(CARGO) build --release --locked -p hyprliquid-bridge --features fault_injection

build/rust-bridge-failure-test: tests/rust-bridge-failure.cpp $(RUST_FAULT_LIB) | build
	$(CXX) $(CXXFLAGS) -UNDEBUG -I$(RUST_FAULT_DIR)/cxxbridge $< $(RUST_FAULT_LIB) -ldl -lpthread -lm -o $@

build/layer-shape-test: tests/layer-shape.cpp src/LayerShape.hpp | build
	$(CXX) $(CXXFLAGS) -UNDEBUG -Isrc $< -o $@

test: build/security-test build/render-test build/rust-differential-test build/rust-bridge-failure-test build/layer-shape-test
	./build/rust-differential-test
	./build/rust-bridge-failure-test
	./build/security-test
	./build/layer-shape-test
	LIBGL_ALWAYS_SOFTWARE=1 ./build/render-test

# Optional disposable-compositor instrumentation; never part of hyprliquid.so.
build/capture-test.so: tests/capture.cpp tests/capture.exports src/PixelTransfer.hpp src/RenderSafety.hpp src/RustCore.hpp build/RustCore.o $(RUST_LIB) Makefile | build
# The fixture must not interpose its numerical helpers on comparison plugins.
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -shared $< $(RUST_LINK) $(RUST_HIDE) -Wl,--version-script=tests/capture.exports $(LDLIBS) -o $@

build/security-test.so: tests/security-render.cpp $(TARGET) | build
	$(CXX) $(CPPFLAGS) -Ibuild $(CXXFLAGS) -shared $< -L. -l:$(TARGET) -Wl,-rpath,'$$ORIGIN/..' $(LDLIBS) -o $@

build/decoration-lifecycle-test.so: tests/decoration-lifecycle.cpp tests/capture.exports | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -shared $< -Wl,--version-script=tests/capture.exports $(LDLIBS) -o $@

# Optional bridge for unloading older installations with stale registrations.
build/reload-guard.so: scripts/reload-guard.cpp tests/capture.exports | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -shared $< -Wl,--version-script=tests/capture.exports $(LDLIBS) -ldl -o $@

build/layer-test: tests/layer.cpp | build
	$(CXX) $(CXXFLAGS) $< -o $@ $$(pkg-config --cflags --libs gtk4 gtk4-layer-shell-0)

build/xdg-shell-client.h: | build
	wayland-scanner client-header $$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml $@

build/xdg-shell-client.c: | build
	wayland-scanner private-code $$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml $@

build/xdg-shell-client.o: build/xdg-shell-client.c
	$(CC) -c $< -o $@ $$(pkg-config --cflags wayland-client)

build/subsurface-test: tests/subsurface.cpp build/xdg-shell-client.h build/xdg-shell-client.o
	$(CXX) $(CXXFLAGS) -Ibuild $< build/xdg-shell-client.o -o $@ $$(pkg-config --cflags --libs wayland-client json-c)

build/demo-scene: demo/render.cpp | build
	$(CXX) $(CXXFLAGS) $< -o $@ $$(pkg-config --cflags --libs egl glesv2)

demo: $(TARGET) build/demo-scene
	python3 scripts/capture-demo.py

-include $(OBJECTS:.o=.d)

.PHONY: all test demo release test-release
