TARGETS=$(patsubst %.c,%,$(shell ls *.c))
CFLAGS+=-I../ -DPTHREAD_SELECT_NUM=1
LDFLAGS+=-lpthread

all:$(TARGETS)

%:%.c

clean:
	$(RM) *.o $(TARGETS)


