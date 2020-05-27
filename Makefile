####### USAGE
# make - buidls with debug options
# make release - builds release options
# make clean - removes object files

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
SRC := $(SRC_DIR)$(wildcard *.cpp) $(wildcard ./oscreceiver_class/*.cpp) $(wildcard ./mtcreceiver_class/*.cpp)
INC := $(wildcard *.h)
OBJ := $(SRC:.cpp=.o)

.PHONY: clean clear

all: debug

release: CXXFLAGS += -O3
release: target

debug: CXXFLAGS += -g -Og
debug: target

target: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LBLIBS)

wipe:
	@clear

clean:
	@rm -rf $(OBJ)