.PHONY: all module tools daemon clean
all: module tools daemon
module:
	$(MAKE) -C kernel
tools:
	gcc -Wall -Wextra -O2 -o tools/bgtest tools/bgtest.c
daemon:
	gcc -Wall -Wextra -O2 -o daemon/bgd daemon/bgd.c
clean:
	$(MAKE) -C kernel clean
	rm -f tools/bgtest daemon/bgd
