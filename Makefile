CC = gcc
CFLAGS = -Wall -O2 -g -Wno-unused-result
OBJS = main.o kernel.o fs.o

toyos: $(OBJS)
	$(CC) $(CFLAGS) -o toyos $(OBJS) -lpthread

%.o: %.c toyos.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f toyos *.o disk.img
