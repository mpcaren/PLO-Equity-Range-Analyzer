# PLO5 equity calculator — GNU make + gcc/clang (use build.bat for MSVC)
CC      ?= gcc
CFLAGS  ?= -O3 -march=native -std=c11 -Wall -Wextra
CFLAGS  += -Isrc
LDLIBS   = -lm

ifeq ($(OS),Windows_NT)
  EXE = .exe
else
  EXE =
  CFLAGS += -pthread
  LDLIBS += -pthread
endif

all: plo5calc$(EXE) plo5tests$(EXE) plo5spr$(EXE)

plo5calc$(EXE): src/main.c src/plo5.c src/plo5.h
	$(CC) $(CFLAGS) -o $@ src/main.c src/plo5.c $(LDLIBS)

plo5tests$(EXE): tests/tests.c src/plo5.c src/plo5.h src/spr.c src/spr.h
	$(CC) $(CFLAGS) -o $@ tests/tests.c src/plo5.c src/spr.c $(LDLIBS)

plo5spr$(EXE): src/sprcli.c src/spr.c src/spr.h src/plo5.c src/plo5.h
	$(CC) $(CFLAGS) -o $@ src/sprcli.c src/spr.c src/plo5.c $(LDLIBS)

# Windows-only GUI + quiz (gcc/MinGW): make gui
gui: plo5gui$(EXE) plo5quiz$(EXE)
plo5gui$(EXE): src/gui.c src/plo5.c src/plo5.h src/plo5gui.rc assets/plo5.ico
	windres src/plo5gui.rc -O coff -o plo5gui_res.o
	$(CC) $(CFLAGS) -mwindows -o $@ src/gui.c src/plo5.c plo5gui_res.o -lgdi32 $(LDLIBS)
plo5quiz$(EXE): src/quiz.c src/plo5.c src/plo5.h src/plo5quiz.rc assets/plo5.ico
	windres src/plo5quiz.rc -O coff -o plo5quiz_res.o
	$(CC) $(CFLAGS) -mwindows -o $@ src/quiz.c src/plo5.c plo5quiz_res.o -lgdi32 $(LDLIBS)

# Windows-only web UI server (gcc/MinGW): make web
web: plo5web$(EXE)
plo5web$(EXE): src/web.c src/plo5.c src/plo5.h src/spr.c src/spr.h
	$(CC) $(CFLAGS) -o $@ src/web.c src/spr.c src/plo5.c -lws2_32 $(LDLIBS)

test: plo5tests$(EXE)
	./plo5tests$(EXE)

bench: plo5calc$(EXE)
	./plo5calc$(EXE) --bench

clean:
	rm -f plo5calc$(EXE) plo5tests$(EXE) plo5gui$(EXE) plo5quiz$(EXE) plo5spr$(EXE) *.o *.obj

.PHONY: all gui test bench clean
