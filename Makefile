CXX ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -fPIC -std=c++23
ifneq ($(filter g++ c++,$(notdir $(CXX))),)
CXXFLAGS += -fno-gnu-unique
endif
CPPFLAGS += $(shell pkg-config --cflags hyprland pixman-1 libdrm)
LDLIBS += $(shell pkg-config --libs hyprland)

TARGET = hyprliquid.so
SOURCES = $(wildcard src/*.cpp)
OBJECTS = $(patsubst src/%.cpp,build/%.o,$(SOURCES))

all: $(TARGET)

build:
	mkdir -p $@

build/%.o: src/%.cpp | build
	$(CXX) $(CPPFLAGS) -Ibuild $(CXXFLAGS) -MMD -MP -c $< -o $@

build/ShaderManager.o: build/ShaderSources.hpp

build/ShaderSources.hpp: shaders/glass.frag shaders/blur.frag scripts/embed-shaders.py | build
	python3 scripts/embed-shaders.py $@

$(TARGET): $(OBJECTS)
	$(CXX) -shared $(LDFLAGS) $^ $(LDLIBS) -o build/$@.tmp
	mv build/$@.tmp $@

build/render-test: tests/render.cpp src/Material.hpp src/MaterialParams.inc src/BuiltInPresets.hpp | build
	$(CXX) $(CXXFLAGS) -Isrc $< -o $@ $$(pkg-config --cflags --libs egl glesv2)

test: build/render-test
	LIBGL_ALWAYS_SOFTWARE=1 ./build/render-test

build/demo-scene: demo/render.cpp | build
	$(CXX) $(CXXFLAGS) $< -o $@ $$(pkg-config --cflags --libs egl glesv2)

demo: $(TARGET) build/demo-scene
	python3 scripts/capture-demo.py

-include $(OBJECTS:.o=.d)

.PHONY: all test demo
