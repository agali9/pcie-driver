CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

all: demo

proto_pcie.o: proto_pcie.cpp proto_pcie.hpp
	$(CXX) $(CXXFLAGS) -c proto_pcie.cpp -o proto_pcie.o

demo: demo.cpp proto_pcie.o
	$(CXX) $(CXXFLAGS) demo.cpp proto_pcie.o -o demo

clean:
	rm -f *.o demo
