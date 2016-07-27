OBJS = cpu.o network.o

cpu: $(OBJS)
	gcc -o $@ $(OBJS) -lauto

%.o : %.c
	gcc -Wall -g -O2 -D__USE_INLINE__ -c $<

clean:
	delete #?.o
