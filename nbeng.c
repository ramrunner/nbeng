/*
 * NonBlocking network engine for tcp/udp/ssl connections
 * The design uses the same socket for sending and receiving
 * data. The relevant data are stored in a connection context
 * which is passed around the funcs for dealing with it.
 * Also provided are 2 sample reading and writing functions.
 *  CopyLeft: (DsP <dsp@2f30.org>, lostd <lostd@2f30.org>)
 * cheers go to photorec for helping recover this file :)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ctype.h>
#include <sys/time.h>
#include <fcntl.h>
#include <err.h>
#include <errno.h>

/* TCP flag */
unsigned int ftcp = 0;

/* flag to know when to quit the prog */
unsigned int quitflag=0;

/* connection context */
typedef struct concontxt_t {
	int confd; /* net connection fd */
	struct addrinfo *clinfo; /* info on where we connect */
	struct addrinfo *srvinfo; /* info for the local part */
} concontxt;

static void
usage(const char *s)
{
        fprintf(stderr, "usage: %s [OPTIONS] "
	                "<remote-addr> <remote-port>\n", s);
        fprintf(stderr, " -t\ttcp session\n");
        fprintf(stderr, " -h\tThis help screen\n");
}

static void
set_nonblocking(int fd)
{
	int opts;
	opts = fcntl(fd, F_GETFL);
	if (opts < 0)
		err(1, "fcntl");
	opts = (opts | O_NONBLOCK);
	if (fcntl(fd, F_SETFL, opts) < 0)
		err(1, "fcntl");
}

static void
set_blocking(int fd)
{
	int opts;
	opts = fcntl(fd, F_GETFL);
	if (opts < 0)
		err(1, "fcntl");
	opts &= (~O_NONBLOCK);
	if (fcntl(fd, F_SETFL, opts) < 0)
		err(1, "fcntl");
}

/* 
 * This is the main function that prepares a socket for
 * a) connection to the other endpoint.
 * b) being able to receive data.
 * when it is ready it sets the fields in the context.
 * XXX: add a flag in the parameter list for type (tcp/udp/ssl)
 */
static void
prepare_socket(concontxt *c, char *to, char *port) 
{
	struct addrinfo cli_hints, *cli_servinfo, srv_hints, *srv_servinfo, *p0;
	int rv, cli_sockfd,optval=1;
	if ((c == NULL) || (to == NULL) || (port == NULL))
		errx(1, "prepare_socket was passed a null arg");
	memset(&cli_hints, 0, sizeof(cli_hints));
	memset(&srv_hints, 0, sizeof(srv_hints));
	cli_hints.ai_family = srv_hints.ai_family = AF_INET;
	cli_hints.ai_socktype = srv_hints.ai_socktype = ftcp ? SOCK_STREAM : SOCK_DGRAM;
	rv = getaddrinfo(to, port, &cli_hints, &cli_servinfo);
	if (rv)
		errx(1, "getaddrinfo: %s", gai_strerror(rv));
	rv = getaddrinfo(NULL, port, &srv_hints, &srv_servinfo);
	if (rv)
		errx(1, "getaddrinfo: %s", gai_strerror(rv));
	/* getaddrinfo returns a list of results so we iterate on it */
	for (p0 = cli_servinfo; p0; p0 = p0->ai_next) {
		cli_sockfd = socket(p0->ai_family, p0->ai_socktype,
				    p0->ai_protocol);
		if (cli_sockfd < 0)
			continue; /* until the socket is ready */
		rv = setsockopt(cli_sockfd, SOL_SOCKET,
				 SO_REUSEADDR, &optval, sizeof(optval));
		if (rv < 0) {
			close(cli_sockfd);
			warn("setsockopt");
			continue;
		}
		break;
	}
	if (!p0)
		errx(1, "failed to create socket");
	/* the same for our local part */
	for (p0 = srv_servinfo; p0; p0 = p0->ai_next) {
		if (bind(cli_sockfd, p0->ai_addr, p0->ai_addrlen) < 0) {
				close(cli_sockfd);
				warn("bind");
				continue;
		}
		break;
	}
	if (!p0)
		errx(1, "failed to bind socket");
	/* all was ok, so we register the socket to the context */
	c->confd = cli_sockfd;
	c->clinfo = cli_servinfo;
}

/*
 * this sample func reads data from input, null-terminates them
 * and sends them over the prepared socket stored in the context.
 * If it gets a "Q" it quits.
 */
static void
stdinputdata(concontxt *con)
{	char *inbuf = NULL, *linbuf = NULL;
	ssize_t inbufln;
        /* deal with input data */
        inbuf = fgetln(stdin, &inbufln);
        if (inbuf[inbufln - 1] == '\n')
                inbuf[inbufln - 1] = '\0';
        else {
                /* EOF without EOL,
                copy and add the NULL */
                if ((linbuf = malloc(inbufln + 1))
                    == NULL) {
                        warnx("linbuf alloc failed");
                        goto freex;
                }
                memcpy(linbuf, inbuf, inbufln);
                linbuf[inbufln] = '\0';
                inbuf = linbuf;
        }
        if ((strncmp(inbuf, "Q", 2) == 0))
                goto freex;
        printf("got msg: %s , sending it\n", inbuf);
	if (ftcp == 0) {
        	sendto(con->confd, inbuf, inbufln, 0,
        	con->clinfo->ai_addr,
        	con->clinfo->ai_addrlen);
	} else {
		/* code for tcp */
	}
        fflush(stdout);
        /* free the buf */
freex:  
	if(linbuf != NULL)
                free(linbuf);
	/* let the main process know we want out */
	quitflag=1;
}

/*
 * This sample receive func, reads up to 512 bytes of input
 * from the socket stored in the context.
 */
static void
srvinputdata(concontxt *con)
{
	ssize_t bytes;
	int ret;
	char host[NI_MAXHOST];
	char buf[512];
	socklen_t addr_len;
	struct sockaddr_storage their_addr;
	addr_len = sizeof(their_addr);
	if (ftcp == 0) {
		bytes = recvfrom(con->confd, buf,
				sizeof(buf), MSG_DONTWAIT,
				(struct sockaddr *)&their_addr,
				&addr_len);
	} else {
		/* code for tcp */
	}
	if (bytes > 0) {
		ret = getnameinfo((struct sockaddr *)&their_addr,
				  addr_len, host,
				  sizeof(host), NULL, 0, 0);
		if (ret < 0) {
				warn("getnameinfo");
				snprintf(host, sizeof(host), "unknown");
		}
		printf("Received %zd bytes from %s that read: %s \n", bytes, host, buf);
	}
}


int 
main(int argc, char *argv[])
{
	/* our input is the stdin for now, we record lines and send them */
	int recfd = STDIN_FILENO;
	char *prog = *argv;
	int c;
	fd_set rsocks;
	int highsock, readsocks;
	struct timeval timeout;
	concontxt *con = (concontxt *) malloc(sizeof (concontxt));
	while ((c = getopt(argc, argv, "ht")) != -1) {
		switch (c) {
		case 'h':
			usage(prog);
			goto freex;
			break;
		case 't':
			ftcp = 1;
			break;
		case '?':
		default:
			goto freex;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 2) {
		usage(prog);
		goto freex;
	}
	if (ftcp == 0) {
		/* prepare the socket for udp */
		prepare_socket(con, argv[0], argv[1]);
	} else {
		/* code for tcp */
	}
	highsock = con->confd;
	/* these are the sockets for reading (stdin + connection fd) */
	while (1) {
		if (quitflag)
			goto freex;
		FD_ZERO(&rsocks);
		FD_SET(recfd, &rsocks);
		FD_SET(con->confd, &rsocks);
		timeout.tv_sec = 1;
		timeout.tv_usec = 0;
		readsocks = select(highsock + 1, &rsocks, (fd_set *)0, 
		  (fd_set *)0, &timeout);
		if (readsocks < 0) {
			warnx("select error");
			goto freex;
		}
		if (readsocks == 0) {
			/* show that we are alive */
			printf(".");
			fflush(stdout);
		} else {
			if (FD_ISSET(recfd, &rsocks)) {
                                stdinputdata(con);
			}
			if (FD_ISSET(con->confd, &rsocks)) {
				srvinputdata(con);
			}
		}
	}

freex:
	printf("exiting\n");
	if (con != NULL) {
		close(con->confd);
		free(con);
	}
	return (0);
}
