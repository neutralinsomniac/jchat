BINS=jchat

all: ${BINS}

jchat: jchat.c
	gcc -o $@ $< -lpthread -lreadline

clean:
	rm -f ${BINS}
