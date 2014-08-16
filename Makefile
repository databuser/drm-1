CFLAGS=-g -c -std=c99 -Wall -m64 -Ofast -flto -march=native -funroll-loops -DLINUX -I/usr/include/libdrm
LDFLAGS=-ldrm
SRCS=main.c
OBJS=$(SRCS:.c=.o)
TARGET=drm-1

all: $(SRCS) $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $@

.c.o:
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -fr $(OBJS) $(TARGET)
