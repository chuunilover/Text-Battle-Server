PORT=26326
CFLAGS = -DPORT=\$(PORT) -g -Wall

all: battle

battle: server.c
	gcc -Wall -o battle server.c $(CFLAGS)
	
clean:
	rm battle
