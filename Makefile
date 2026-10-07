CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic

.PHONY: all run demo test clean
all: pcb_inspector

pcb_inspector: pcb_inspector.c
	$(CC) $(CFLAGS) $< -o $@

run: pcb_inspector
	./pcb_inspector

demo: pcb_inspector
	./pcb_inspector --step --mode cpu --quantum 1000 --bursts 8,4,12 --unit-ms 250

test: pcb_inspector
	python3 tests/test_demo.py

clean:
	rm -f pcb_inspector
