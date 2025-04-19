CXX = g++
CXXFLAGS = -std=c++20 -O2 -DGL_SILENCE_DEPRECATION

INCLUDES = -Iimgui -Iimgui/backends \
           -I/opt/homebrew/include \
           -I/opt/homebrew/include/GLFW

LDFLAGS = -L/opt/homebrew/lib \
          -lglfw -lcurl \
          -framework Cocoa -framework OpenGL -framework IOKit -framework CoreVideo

IMGUI_SRCS = $(wildcard imgui/*.cpp) \
             imgui/backends/imgui_impl_glfw.cpp \
             imgui/backends/imgui_impl_opengl3.cpp

SRCS = edit.cpp $(IMGUI_SRCS)
OBJS = $(SRCS:.cpp=.o)

TARGET = imgui_app

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ $(LDFLAGS) -o $@

clean:
	rm -f $(TARGET) $(OBJS)

