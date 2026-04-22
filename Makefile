CFLAGS = -Wno-int-conversion
.PHONY: all
all:
	gcc $(CFLAGS) -o code main.c buddy.c