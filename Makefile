BINS=jchat newchat

all: ${BINS}

jchat: jchat.c
	gcc -o $@ $< -lpthread -lreadline

newchat: newchat.c
	gcc -o $@ $< -lpthread -lreadline

clean:
	rm -f ${BINS}
