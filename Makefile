.PHONY: all module tools daemon cli clean
all: module tools daemon cli
module:
	$(MAKE) -C kernel
tools:
	gcc -Wall -Wextra -O2 -o tools/bgtest tools/bgtest.c
	gcc -Wall -Wextra -O2 -o tools/bgfault tools/bgfault.c
daemon:
	gcc -Wall -Wextra -O2 -o daemon/bgd daemon/bgd.c
cli:
	gcc -Wall -Wextra -O2 -o cli/bgctl cli/bgctl.c
clean:
	$(MAKE) -C kernel clean
	rm -f tools/bgtest tools/bgfault daemon/bgd cli/bgctl
