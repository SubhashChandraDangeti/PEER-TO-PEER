# Builds ./tracker. Shared code in ../common is compiled into this binary.
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -pthread
CPPFLAGS += -I../common
LDLIBS   += -lcrypto -pthread

# macOS ships no OpenSSL headers: use Homebrew's (brew install openssl@3).
ifeq ($(shell uname -s),Darwin)
  OPENSSL ?= $(shell brew --prefix openssl@3 2>/dev/null)
  CPPFLAGS += -I$(OPENSSL)/include
  LDLIBS   := -L$(OPENSSL)/lib $(LDLIBS)
endif

SRCS := $(wildcard *.cpp) $(wildcard ../common/*.cpp)
OBJS := $(patsubst %.cpp,obj/%.o,$(notdir $(SRCS)))
vpath %.cpp . ../common

tracker: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

obj/%.o: %.cpp | obj
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

obj:
	mkdir -p obj

clean:
	rm -rf obj tracker

-include $(OBJS:.o=.d)
.PHONY: clean
