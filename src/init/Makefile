TARGET=init
CSOURCES = $(shell echo *.c)
SSOURCES = $(shell echo *.s)

OBJECTS = $(CSOURCES:.c=_c.o)
OBJECTS += $(SSOURCES:.S=_S.o)
LDFLAGS = -lmy -L$(MYOS_PATH)/lib

all: $(TARGET)
$(TARGET): $(OBJECTS)
  $(CC) $(LDFLAGS) $(FLAGS) -o $(TARGET) $(OBJECTS)
  
include ../base.mk
