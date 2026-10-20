CXX ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -fPIC -std=c++23
ifneq ($(filter g++ c++,$(notdir $(CXX))),)
CXXFLAGS += -fno-gnu-unique
endif
CPPFLAGS += $(shell pkg-config --cflags hyprland pixman-1 libdrm json-c)
LDLIBS += $(shell pkg-config --libs hyprland json-c)

TARGET = hyprliquid.so
SOURCES = $(wildcard src/*.cpp)
OBJECTS = $(patsubst src/%.cpp,build/%.o,$(SOURCES))

all: $(TARGET)

build:
	mkdir -p $@

build/%.o: src/%.cpp | build
	$(CXX) $(CPPFLAGS) -Ibuild $(CXXFLAGS) -MMD -MP -c $< -o $@

build/ShaderManager.o: build/ShaderSources.hpp

build/ShaderSources.hpp: shaders/glass.frag shaders/blur.frag shaders/scene-sample.frag shaders/clean-backdrop.glsl scripts/embed-shaders.py | build
	python3 scripts/embed-shaders.py $@

$(TARGET): $(OBJECTS)
	$(CXX) -shared $(LDFLAGS) $^ $(LDLIBS) -o build/$@.tmp
	mv build/$@.tmp $@

build/render-test: tests/render.cpp src/Material.hpp src/MaterialParams.inc src/BuiltInPresets.hpp src/PixelTransfer.hpp | build
	$(CXX) $(CXXFLAGS) -Isrc $< -o $@ $$(pkg-config --cflags --libs egl glesv2)

build/security-test: tests/security.cpp src/RenderSafety.hpp src/Material.hpp src/MaterialParams.inc src/LuaSafety.hpp | build
	$(CXX) $(CXXFLAGS) -UNDEBUG -Isrc $< -o $@ $$(pkg-config --cflags --libs lua)

test: build/security-test build/render-test
	./build/security-test
	LIBGL_ALWAYS_SOFTWARE=1 ./build/render-test

# Optional disposable-compositor instrumentation; never part of hyprliquid.so.
build/capture-test.so: tests/capture.cpp src/PixelTransfer.hpp src/RenderSafety.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -shared $< $(LDLIBS) -o $@

build/security-test.so: tests/security-render.cpp $(TARGET) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -shared $< -L. -l:$(TARGET) -Wl,-rpath,'$$ORIGIN/..' $(LDLIBS) -o $@

build/layer-test: tests/layer.cpp | build
	$(CXX) $(CXXFLAGS) $< -o $@ $$(pkg-config --cflags --libs gtk4 gtk4-layer-shell-0)

build/demo-scene: demo/render.cpp | build
	$(CXX) $(CXXFLAGS) $< -o $@ $$(pkg-config --cflags --libs egl glesv2)

demo: $(TARGET) build/demo-scene
	python3 scripts/capture-demo.py

-include $(OBJECTS:.o=.d)

.PHONY: all test demo
