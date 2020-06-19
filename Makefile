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
LBLIBS = -lrtaudio -lrtmidi -lpthread -lstdc++fs -loscpack

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

$(TARGET): $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LBLIBS)

clean:
	@rm -rf $(OBJ) $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(prefix)/bin/
	install -m 755 $(TARGET) $(DESTDIR)$(prefix)/bin/