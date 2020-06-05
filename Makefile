####### USAGE
# make - buidls with debug options
# make release - builds release options
# make clean - removes object files

#PREFIX is environment variable, but if it is not set, then set default value
ifeq ($(prefix),)
    prefix := /usr/local
endif

#Compiler, compiler flags and linker flags
CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17

# User preprocessor defines
CXXFLAGS += -D__UNIX_JACK__

LDFLAGS =  -fsanitize=address
LBLIBS = -lrtaudio -lrtmidi -lpthread -lstdc++fs \
			./oscreceiver_class/oscpack/osc/OscReceivedElements.o \
			./oscreceiver_class/oscpack/ip/posix/UdpSocket.o

TARGET := audioplayer
SRC := $(wildcard *.cpp) \
			$(wildcard ./oscreceiver_class/*.cpp) \
			$(wildcard ./mtcreceiver_class/*.cpp) \
			$(wildcard ./sysqlogger_class/*.cpp) \

INC := $(wildcard *.h)
OBJ := $(SRC:.cpp=.o)

.PHONY: clean clear

all: debug

release: CXXFLAGS += -O3
release: target

debug: CXXFLAGS += -g -Og
debug: target

target: $(TARGET)

$(TARGET): $(OBJ) oscpack
	$(CXX) $(OBJ) -o $@ $(LBLIBS)

oscpack:
	@echo Checking oscpack objects to be built
	@cd oscreceiver_class/oscpack/ 1> /dev/null && make -i 1> /dev/null
	@cd ../.. 1> /dev/null

wipe:
	@clear

clean:
	@rm -rf $(OBJ)

install: audioplayer
	install -d $(DESTDIR)$(prefix)/bin/
	install -m 644 audioplayer $(DESTDIR)$(prefix)/bin/