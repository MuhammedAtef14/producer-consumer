CXX = g++
CXXFLAGS = -std=c++11 -Wall -pthread

all: producer consumer

producer: producer.cpp
	$(CXX) $(CXXFLAGS) -o producer producer.cpp

consumer: consumer.cpp
	$(CXX) $(CXXFLAGS) -o consumer consumer.cpp

clean:
	rm -f producer consumer
	ipcrm -a

.PHONY: all clean