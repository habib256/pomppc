# Makefile — plugin GA de POMPPC (POMPPCGA.plugin), à compiler DANS l'invité
# Tiger (gcc 4.0, SDK 10.4u). Quartz Extreme : docs/quartz-extreme.md.
SDK     = /Developer/SDKs/MacOSX10.4u.sdk
CC      = /usr/bin/gcc-4.0
CFLAGS  = -arch ppc -isysroot $(SDK) -O2 -Wall -Wno-unused-parameter -fno-common -g
LDFLAGS = -arch ppc -isysroot $(SDK) -bundle
LIBS    = -framework IOKit -framework CoreFoundation

NAME    = POMPPCGA
BIN     = $(NAME).plugin/Contents/MacOS/$(NAME)

all: $(BIN) $(NAME).plugin/Contents/Info.plist

$(BIN): $(NAME).o
	mkdir -p $(NAME).plugin/Contents/MacOS
	$(CC) $(LDFLAGS) -o $@ $(NAME).o $(LIBS)

$(NAME).plugin/Contents/Info.plist: Info.plist
	mkdir -p $(NAME).plugin/Contents
	cp Info.plist $@

clean:
	rm -rf $(NAME).o $(NAME).plugin
