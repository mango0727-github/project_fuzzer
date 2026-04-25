CC = clang
CXX = clang++
AR = ar

TARGET = libobj14hook.a

C_SOURCES = \
	coverage.c \
	corpus.c \
	main.c \
	mutator.c \
	runtime.c \
	trace_counter_obj14.c

CXX_SOURCES = target_shim.cc
HEADERS = fuzzer_internal.h

OBJECTS = $(C_SOURCES:.c=.o) $(CXX_SOURCES:.cc=.o)

# step1 and for release. aggressive error checking
CFLAGS_FOR_STEP1 = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wundef -Wstrict-prototypes

# step5. dynamic analysis tools ASan and UBSan
CFLAGS_FOR_STEP5 = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-optimize-sibling-calls

# Release build flag
CFLAGS_FOR_RELEASE = -O3

CFLAGS_COMMON =
CXXFLAGS_COMMON =

.PHONY: all clean fuzzer_step1 fuzzer_step3 llvm_testing_step5 fuzzer_release_step5

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(AR) rcs $@ $^

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_EXTRA) -c $< -o $@

%.o: %.cc $(HEADERS)
	$(CXX) $(CXXFLAGS_COMMON) $(CXXFLAGS_EXTRA) -c $< -o $@

# step 1
fuzzer_step1: clean
	$(MAKE) $(TARGET) CFLAGS_EXTRA="$(CFLAGS_FOR_STEP1)" CXXFLAGS_EXTRA="$(CFLAGS_FOR_STEP1)"

# for step3
fuzzer_step3: clean
	$(MAKE) $(TARGET) CFLAGS_EXTRA="$(CFLAGS_FOR_RELEASE)" CXXFLAGS_EXTRA="$(CFLAGS_FOR_RELEASE)"

# for step5
llvm_testing_step5: clean
	$(MAKE) $(TARGET) CFLAGS_EXTRA="$(CFLAGS_FOR_STEP5)" CXXFLAGS_EXTRA="$(CFLAGS_FOR_STEP5)"

# for release
fuzzer_release_step5: clean
	$(MAKE) $(TARGET) CFLAGS_EXTRA="$(CFLAGS_FOR_RELEASE) $(CFLAGS_FOR_STEP1)" CXXFLAGS_EXTRA="$(CFLAGS_FOR_RELEASE) $(CFLAGS_FOR_STEP1)"

clean:
	rm -f $(OBJECTS) $(TARGET)
