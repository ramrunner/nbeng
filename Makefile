BIN = nbeng
VER = 0.1-alpha
SRC = nbeng.c
OBJ = ${SRC:.c=.o}

CC = gcc

INCS = -I/usr/local/include
LIBS = -L/usr/local/lib

CFLAGS += -Wall -Wextra -Wunused -DVERSION=\"${VER}\" ${INCS}
#add assl
#LDFLAGS += -lassl ${LIBS}

$(BIN): ${OBJ}
	${CC} ${CFLAGS} ${LDFLAGS} -o $@ ${OBJ}

%.o: %.c
	${CC} ${CFLAGS} -c -o $@ $<

clean:
	rm -rf ${BIN} ${OBJ}

all: nbeng
