OBJS = cpu.o network.o
NS = cpu_nonstripped

cpu: $(OBJS)
	gcc -o $(NS) $(OBJS) -lauto -N
	strip $(NS) -o $@
	
%.o : %.c
	gcc -Wall -g -O2 -D__USE_INLINE__ -c $<

clean:
	delete #?.o
