CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic
LDLIBS ?= -lcrypto

TARGET := hardlink_dedup
ifeq ($(OS),Windows_NT)
	TARGET := hardlink_dedup.exe
endif

.PHONY: all clean

all: $(TARGET)

$(TARGET): main.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET)
