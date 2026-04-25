CC = clang
CXX = clang++
AR = ar

TARGET = libmainhook.a

C_SOURCES = \
	coverage.c \
	corpus.c \
	main.c \
	mutator.c \
	runtime.c \
	trace_counter.c

CXX_SOURCES = target_shim.cc
HEADERS = fuzzer_internal.h

OBJECTS = $(C_SOURCES:.c=.o) $(CXX_SOURCES:.cc=.o)

# Default warning-oriented build flags.
CFLAGS_FOR_WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wundef -Wstrict-prototypes

# Release build flags.
CFLAGS_FOR_RELEASE = -O3

CFLAGS_COMMON = $(CFLAGS_FOR_RELEASE) $(CFLAGS_FOR_WARNINGS)
CXXFLAGS_COMMON = $(CFLAGS_FOR_RELEASE) $(CFLAGS_FOR_WARNINGS)

.PHONY: all clean

all: clean $(TARGET)

$(TARGET): $(OBJECTS)
	$(AR) rcs $@ $^

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_EXTRA) -c $< -o $@

%.o: %.cc $(HEADERS)
	$(CXX) $(CXXFLAGS_COMMON) $(CXXFLAGS_EXTRA) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)
