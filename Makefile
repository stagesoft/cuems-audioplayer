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
			./oscreceiver/oscpack/osc/OscReceivedElements.o \
			./oscreceiver/oscpack/ip/posix/UdpSocket.o

TARGET := audioplayer-cuems
SRC := $(wildcard *.cpp) \
			$(wildcard ./oscreceiver/*.cpp) \
			$(wildcard ./mtcreceiver/*.cpp) \
			$(wildcard ./cuemslogger/*.cpp) \

INC := $(wildcard *.h)
OBJ := $(SRC:.cpp=.o)

.PHONY: clean

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
	@cd oscreceiver/oscpack/ 1> /dev/null && make -i 1> /dev/null
	@cd ../.. 1> /dev/null

clean:
	@rm -rf $(OBJ) $(TARGET)
	@cd oscreceiver/oscpack/ 1> /dev/null && make -i clean 1> /dev/null
	@cd ../.. 1> /dev/null

install: $(TARGET)
	install -d $(DESTDIR)$(prefix)/bin/
	install -m 755 $(TARGET) $(DESTDIR)$(prefix)/bin/