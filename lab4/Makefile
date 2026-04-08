CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++11 -O2
TARGETS = client server
SOURCES = client.cpp server.cpp

all: $(TARGETS)
client: client.cpp
	$(CXX) $(CXXFLAGS) -o client client.cpp

server: server.cpp
	$(CXX) $(CXXFLAGS) -o server server.cpp

clean:
	rm -f $(TARGETS)

.PHONY: all clean