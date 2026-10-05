# ASCIIArt - desktop app and terminal viewer
#
#   make            build asciiapp.exe
#   make run        build and open a sample
#   make samples    regenerate the sample images and clips
#   make icon       rebuild the application icon
#   make shortcut   add the Start menu entry
#   make test       run the Python test suite
#   make clean      remove build output
#
# Needs gcc (MSYS2/MinGW), and ffmpeg on PATH for video.

# Some make builds hand recipes an empty environment. Two things break:
#   TMP/TEMP empty  -> gcc falls back to C:\Windows and cannot write there
#   APPDATA empty   -> Python cannot resolve its per-user site-packages, so
#                      `make test` reports numpy missing when it is installed
# Both are restored here rather than being left to the caller's shell.
TMPROOT      := $(CURDIR)/.buildtmp
export TMPDIR = $(TMPROOT)
export TMP    = $(TMPROOT)
export TEMP   = $(TMPROOT)

ifeq ($(strip $(APPDATA)),)
  ifneq ($(strip $(USERPROFILE)),)
    export APPDATA := $(USERPROFILE)\AppData\Roaming
  else ifneq ($(strip $(HOMEDRIVE)$(HOMEPATH)),)
    export APPDATA := $(HOMEDRIVE)$(HOMEPATH)\AppData\Roaming
  else
    export APPDATA := C:\Users\$(USERNAME)\AppData\Roaming
  endif
endif

CC      := gcc
CFLAGS  := -O2 -Wall -municode -mwindows
LDLIBS  := -lgdi32 -lcomdlg32 -lshell32 -lm
TARGET  := asciiapp.exe
SRC     := asciiapp.c
ICON    := asciiapp.ico
RES     := asciiapp_res.o
PYTHON  := python

# windres is optional: without it the app builds, just with the stock icon.
WINDRES := $(shell command -v windres 2>/dev/null)
ifeq ($(WINDRES),)
  RESOBJ :=
else
  RESOBJ := $(RES)
endif

.PHONY: all run samples icon shortcut test clean help

all: $(TARGET)

$(TARGET): $(SRC) $(RESOBJ)
	@mkdir -p $(TMPROOT)
	@echo "  [cc]  $(SRC)"
	@$(CC) $(CFLAGS) -o $@ $(SRC) $(RESOBJ) $(LDLIBS)
	@echo "  [ok]  $@"

$(RES): asciiapp.rc $(ICON)
	@echo "  [rc]  asciiapp.rc"
	@windres asciiapp.rc -O coff -o $@

$(ICON):
	@echo "  [py]  make_icon.py"
	@$(PYTHON) make_icon.py

icon:
	@$(PYTHON) make_icon.py

run: $(TARGET)
	@./$(TARGET) samples/sphere.png

samples:
	@$(PYTHON) samples/make_samples.py

shortcut: $(TARGET)
	@powershell -ExecutionPolicy Bypass -File install-shortcut.ps1

test:
	@$(PYTHON) selftest.py

clean:
	@rm -rf $(TARGET) $(RES) $(TMPROOT)
	@echo "  [rm]  build output"

help:
	@echo "targets: all run samples icon shortcut test clean"
