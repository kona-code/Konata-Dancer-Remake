# --- toolchain -----------------------------------------------------
CXX       	:= g++
CC			:= clang
PKG_CONFIG	:= pkg-config

# --- directories ---------------------------------------------------
SRCDIR     := src
OBJDIR     := obj
BINDIR     := output

# --- flags ---------------------------------------------------------
CXXFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2 gtk+-3.0 appindicator3-0.1)
CXXFLAGS += -Iinclude -I/usr/include/glib-2.0 -I/usr/include/gtk-3.0
CFLAGS   := -std=c17      -O2 -g `pkg-config --cflags sdl2` -I$(SRCDIR)

LDFLAGS  := $(shell $(PKG_CONFIG) --libs sdl2 gtk+-3.0 appindicator3-0.1)
LDFLAGS  +=  -limgui -lvulkan

# --- sources -------------------------------------------------------
SRC_FILES     := $(wildcard $(SRCDIR)/*.cpp)
SRC_FILES_C   := $(wildcard $(SRCDIR)/*.c)
SOURCES       := $(SRC_FILES)

OBJECTS_CPP       := $(patsubst %.cpp,$(OBJDIR)/%.o,$(notdir $(SOURCES)))
OBJECTS_C   := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRC_FILES_C))
OBJECTS := $(OBJECTS_CPP) $(OBJECTS_C)
TARGET        := $(BINDIR)/main

# --- default rule --------------------------------------------------
.PHONY: all
all: $(TARGET)

# --- linking -------------------------------------------------------
$(TARGET): $(OBJECTS) | $(BINDIR)
	@echo "Linking $@"
	$(CXX) $(addprefix $(OBJDIR)/,$(notdir $(OBJECTS))) -o $@ $(LDFLAGS)

# --- compilation ---------------------------------------------------
# from src/
$(OBJDIR)/%.o: $(SRCDIR)/%.cpp | $(OBJDIR)
	@echo "Compiling $<"
	$(CXX) $(CXXFLAGS) -c $< -o $@
# for C
$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
# --- dirs ----------------------------------------------------------
$(OBJDIR):
	mkdir -p $(OBJDIR)

$(BINDIR):
	mkdir -p $(BINDIR)

# --- utils ---------------------------------------------------------
.PHONY: run
run: all
	@echo "Running $(TARGET)"
	$(TARGET)

.PHONY: clean
clean:
	rm -rf $(OBJDIR) $(BINDIR)
