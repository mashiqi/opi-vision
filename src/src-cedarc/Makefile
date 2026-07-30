CC ?= gcc
CPPFLAGS += -Ivendor/include
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS += -lvencoder -lMemAdapter -lpthread -lm -lrt -ldl

TARGET := ../bin/aw-h264-encoder
SOURCE := aw-h264-encoder.c
REQUIRED_HEADERS := vendor/include/vencoder.h vendor/include/memoryAdapter.h

.PHONY: all clean check-vendor

all: check-vendor $(TARGET)

check-vendor:
	@test -f $(SOURCE) || { echo "缺少源码: $(SOURCE)" >&2; exit 1; }
	@for file in $(REQUIRED_HEADERS); do \
		test -f $$file || { echo "缺少Cedarc头文件: $$file" >&2; exit 1; }; \
	done

$(TARGET): $(SOURCE) $(REQUIRED_HEADERS)
	@mkdir -p ../bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCE) $(LDLIBS)

clean:
	$(RM) $(TARGET)
