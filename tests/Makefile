CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -pthread
CPPFLAGS += -I../common
LDLIBS   += -lcrypto -pthread

# macOS ships no OpenSSL headers: use Homebrew's (brew install openssl@3).
ifeq ($(shell uname -s),Darwin)
  OPENSSL ?= $(shell brew --prefix openssl@3 2>/dev/null)
  CPPFLAGS += -I$(OPENSSL)/include
  LDLIBS   := -L$(OPENSSL)/lib $(LDLIBS)
endif

COMMON := $(wildcard ../common/*.cpp)

unit_tests: unit_tests.cpp $(COMMON)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f unit_tests

.PHONY: clean
