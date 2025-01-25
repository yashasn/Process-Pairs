all: watch_dog

watch_dog: watch_dog.cc
	g++ $< -o $@
	
run: watch_dog
	./watch_dog url.lst 2000 5000
	
clean:
	rm -r watch_dog
