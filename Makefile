.DEFAULT_GOAL := all

SRC_RECEIVER = decode_video_zmq.cpp
SRC_SENDER = encode_video_zmq.cpp
CXX_HDRS = avutils.hpp avtransmitter.hpp
CXX_FLAGS = -std=c++14 -g -O0
LIBS = -lavformat -lswscale -lavcodec -lavresample -lavutil -lavdevice -lzmq -lopencv_core -lopencv_imgproc -lopencv_highgui -lopencv_imgcodecs -lc++
INCLUDE_DIRS = -I /usr/local/include/opencv4/ -I /usr/local/include/
LIBRARY_DIRS = -L /usr/local/lib/
CXX = clang++
EXE_NAME_SENDER = encode_video_zmq
EXE_NAME_RECEIVER = decode_video_zmq

$(EXE_NAME_SENDER): $(SRC_SENDER) $(CXX_HDRS)
	$(CXX) $(CXX_FLAGS) $(INCLUDE_DIRS) $(LIBRARY_DIRS) -o $(EXE_NAME_SENDER) $(LIBS) $(SRC_SENDER)

$(EXE_NAME_RECEIVER): $(SRC_RECEIVER) $(CXX_HDRS)
	$(CXX) $(CXX_FLAGS) $(INCLUDE_DIRS) $(LIBRARY_DIRS) -o $(EXE_NAME_RECEIVER) $(LIBS) $(SRC_RECEIVER)

.PHONY: all

all: $(EXE_NAME_SENDER) $(EXE_NAME_RECEIVER)
