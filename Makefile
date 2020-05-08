#Compiler, compiler flags and linker flags
CXX = g++
#CXXFLAGS = -Wall -Werror -Wextra -std=c++17 -O3 -g -I . -I ..
CXXFLAGS = -Wall -Wextra -std=c++17 -O3 -g -D__UNIX_JACK__
#-D__LINUX_ALSA__
#-D__UNIX_JACK__
LDFLAGS =  -fsanitize=address
LBLIBS = -lrtaudio -lrtmidi -lpthread -lstdc++fs \
			./oscreceiver_class/oscpack/osc/OscReceivedElements.o \
			./oscreceiver_class/oscpack/ip/posix/UdpSocket.o

# Version ids

NAME := audioplayer
SRC := $(wildcard *.cpp) $(wildcard ./oscreceiver_class/*.cpp) $(wildcard ./mtcreceiver_class/*.cpp)
INC := $(wildcard *.h)
OBJ := $(SRC:.cpp=.o)

.PHONY: clean

all: app

app: $(NAME)

$(NAME): clear $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LBLIBS)

clear:
	@clear

clean:
	rm -rf $(OBJ)