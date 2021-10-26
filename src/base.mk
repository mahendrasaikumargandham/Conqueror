SHELL ?= /bin/sh
CC ?= gcc
FLAGS ?= -nostdlib -I../include
MYOS_PATH ?= /mnt/Conqueror
DESTDIR ?= /mnt/Conqueror

PREFIX ?= $(DESTDIR)
BINDIR ?= $(PREFIX)/sbin

OBJECTS += ../crt0_s.o
%_c.o : %.c
  $(CC) $(FLAGS) -c $< -o $@
  
%_s.o : %.s
  $(CC) $(FLAGS) -c $< -o $@
  
install:
  cp $(TARGET) $(BINDIR)/

clean:
  rm *.o $(TARGET)