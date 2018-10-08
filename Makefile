BINS=jchat

all: ${BINS}

jchat: jchat.c jchat.h
	gcc -o $@ jchat.c -lpthread -lreadline

clean:
	rm -f ${BINS}
