CFLAGS ?= -O2 -Wall
PKGS := libpulse

swaybar: swaybar.c
	$(CC) $(CFLAGS) -o $@ swaybar.c $(shell pkg-config --cflags --libs $(PKGS))

clean:
	rm -f swaybar

.PHONY: clean
