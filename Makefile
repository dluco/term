CC = gcc
CFLAGS += -g -Wall
LDFLAGS += -lX11 -lutil

SRC = term.c
OBJ = ${SRC:.c=.o}

all: term

.c.o:
	${CC} -c ${CFLAGS} $<

${OBJ}: config.h

term: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

clean:
	${RM} term ${OBJ}

.PHONY: all clean
