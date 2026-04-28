# GPU Display Driver - Server/Client Architecture
# Server: GPU 1080p 160Hz + Virtual Sound Card + Network Streaming
# Client: Remote Display Receiver + Audio Playback

CXX = gcc
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
LDFLAGS_SERVER = -lvulkan -lasound -lpthread -lX11 -lXtst -lXrandr -lstdc++ -lm
LDFLAGS_CLIENT = -lasound -lpthread -lX11 -lstdc++ -lm

SERVER_TARGET = server
CLIENT_TARGET = client

.PHONY: all clean run-server run-client

all: $(SERVER_TARGET) $(CLIENT_TARGET)

$(SERVER_TARGET): server.cpp
	@echo "Compiling server..."
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS_SERVER)
	@echo "Server build complete!"

$(CLIENT_TARGET): client.cpp
	@echo "Compiling client..."
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS_CLIENT)
	@echo "Client build complete!"

clean:
	@echo "Cleaning..."
	rm -f $(SERVER_TARGET) $(CLIENT_TARGET)
	@echo "Clean complete."

run-server: $(SERVER_TARGET)
	./$(SERVER_TARGET)

run-client: $(CLIENT_TARGET)
	./$(CLIENT_TARGET)
