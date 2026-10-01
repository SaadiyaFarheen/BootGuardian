.PHONY: all module tools clean
all: module tools
module:
	$(MAKE) -C kernel
tools:
	gcc -Wall -Wextra -O2 -o tools/bgtest tools/bgtest.c
clean:
	$(MAKE) -C kernel clean
	rm -f tools/bgtest
