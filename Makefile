BINS=jchat newchat

all: ${BINS}

jchat: jchat.c
	gcc -o $@ $< -lpthread -lreadline

newchat: newchat.c newchat.h
	gcc -o $@ newchat.c -lpthread -lreadline

clean:
	rm -f ${BINS}
