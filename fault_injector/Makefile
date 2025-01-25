INJECTORS=fault_injector.so fork_failed.so 2socket_crash.so

all: fault_injector.so

fault_injector.so: fault_injector.cc
	g++ -c -fpic $< -o fault_injector.o
	g++ -lrt -shared -o $@ fault_injector.o -ldl

clean:
	rm -f fault_injector.so fault_injector.o
