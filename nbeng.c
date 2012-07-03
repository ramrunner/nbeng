/*
 * Non-blocking network engine for tcp/udp/ssl connections
 * The design uses the same socket for sending and receiving
 * data.  The relevant data are stored in a connection context
 * which is passed around the functions for dealing with it.
 * Also provided are sample reading and writing functions.
 * Copyleft:
 *    DsP <dsp@2f30.org>
 *    Lazaros Koromilas <lostd@2f30.org>
 * Cheers go to PhotoRec for helping recover this file :)
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <netdb.h>

#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* global flags */
unsigned int tflag = 0;
unsigned int sflag = 0;
unsigned int qflag = 0;

/* connection context */
typedef struct concontxt_t {
#define UDPCON 1
#define TCPCON 2
#define SSLCON 3
	unsigned int contype; /* connection type */
	int confd; /* net connection fd */
	int clifd;
	int clifds[FD_SETSIZE]; /* for tcp connected clients */
	fd_set lset; /* the set that we select() on */
	struct addrinfo *clinfo; /* info on where we connect */
	struct addrinfo *srvinfo; /* info for the local part */
#define INBUFSIZ 512
	char *buf; /* buffer used for input/output */
	unsigned int buflen;
	int wfd, lfd; /* local file descriptors */
} concontxt;

void
usage(int ret)
{
        fprintf(stderr, "usage: nbeng [-ht] rhost rport\n"
                        "\t-t	Use TCP for transport\n"
                        "\t-h	This help screen\n");
	if (ret)
		exit(1);
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
 * This is the main function that prepares a socket for:
 *   a) connection to the other endpoint.
 *   b) being able to receive data.
 * When it is ready it sets the fields in the context.
 * The flag for the connection type lives in the context.
 */
static void
prepare_socket(concontxt *con, char *host, char *port, int lflag) 
{
	struct addrinfo cli_hints, *cli_servinfo, srv_hints, *srv_servinfo, *p0;
	int rv, cli_sockfd, optval = 1;

	if ((con == NULL) || (host == NULL) || (port == NULL))
		errx(1, "prepare_socket was passed a null arg");
	memset(&cli_hints, 0, sizeof (cli_hints));
	memset(&srv_hints, 0, sizeof (srv_hints));
	cli_hints.ai_family = srv_hints.ai_family = AF_INET;
	srv_hints.ai_flags = AI_PASSIVE;
	cli_hints.ai_socktype = srv_hints.ai_socktype =
	    ((con->contype == TCPCON) || (con->contype == SSLCON))
	    ? SOCK_STREAM : SOCK_DGRAM;
	rv = getaddrinfo(host, port, &cli_hints, &cli_servinfo);
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
				 SO_REUSEADDR, &optval, sizeof (optval));
		if (rv < 0) {
			close(cli_sockfd);
			warn("setsockopt");
			continue;
		}
		break;
	}
	if (!p0)
		err(1, "socket");
	/* the same for our local part */
	if (lflag) {
	for (p0 = srv_servinfo; p0; p0 = p0->ai_next) {
		if (bind(cli_sockfd, p0->ai_addr, p0->ai_addrlen) < 0) {
				close(cli_sockfd);
				warn("bind");
				continue;
		}
		break;
	}
	if (!p0)
		err(1, "bind");
	}
	/* if it's a tcp connection we have to listen() */
	if (con->contype != UDPCON && lflag)
		listen(cli_sockfd, 5);
	/* all was ok, so we register the socket to the context */
	con->confd = cli_sockfd;
	con->clinfo = cli_servinfo;
}

/*
 * Reads data from standard input and saves them in the context.
 * Sets the qflag on "Q".
 */
static void
readstdin(concontxt *con)
{
	char buf[INBUFSIZ];
	ssize_t n;

	/* handle input data */
	n = read(con->wfd, buf, INBUFSIZ);
	buf[n] = '\0';

	/* user quits? */
	if (n == 0 || strncmp(buf, "Q\n", 3) == 0) {
		qflag = 1;
		return;
	}
       	printf("n=%zd buf=%s", n, buf);

	/* copy to context */
	if ((con->buf = (char *)malloc(n)) == NULL) {
		warn("malloc");
		goto freex;
	}
	memcpy(con->buf, buf, n);
	con->buflen = n;
	return;

freex:
	if (con->buf != NULL)
                free(con->buf);
	con->buf = NULL;
	con->buflen = 0;
	return;
}

/*
 * Sends data available in the context to all peers.
 */
static void
writesock(concontxt *con)
{
	ssize_t n;

	/* nothing to do */
	if (con->buf == NULL)
		return;

	if (con->contype == UDPCON) {
        	n = sendto(con->confd, con->buf, con->buflen, 0,
        	    con->clinfo->ai_addr,
        	    con->clinfo->ai_addrlen);
		if (n < 0)
			warn("sendto");
	} else {
		n = write(con->clifd, con->buf, con->buflen);
		if (n < 0) {
			warn("write");
			close(con->clifd);
			con->clifd = -1;
			goto freex;
		}
	}

freex:
	if (con->buf != NULL)
                free(con->buf);
	con->buf = NULL;
	con->buflen = 0;
	return;
}

/*
 * Reads data from the socket and saves them in the context.
 */
static void
readsock(concontxt *con)
{
	char buf[INBUFSIZ];
	ssize_t n;
	int r;
	char host[NI_MAXHOST];
	socklen_t addr_len;
	struct sockaddr_storage their_addr;

	addr_len = sizeof (their_addr);
	if (con->contype == UDPCON) {
		n = recvfrom(con->confd, buf, sizeof (buf), MSG_DONTWAIT,
		    (struct sockaddr *)&their_addr, &addr_len);
		if (n < 0) {
			warn("recvfrom");
			goto freex;
		}

		/* resolv */
		r = getnameinfo((struct sockaddr *)&their_addr, addr_len,
		    host, sizeof (host), NULL, 0, 0);
		if (r < 0) {
			warn("getnameinfo");
			snprintf(host, sizeof (host), "unknown");
		}
		printf("host=%s\n", host);
	} else {
		n = read(con->clifd, buf, sizeof (buf));
		if (n == 0) {
			qflag = 1;
			close(con->clifd);
			con->clifd = -1;
			goto freex;
		}
	}
	buf[n] = '\0';

	/* copy to context */
	if ((con->buf = (char *)malloc(n)) == NULL) {
		warn("malloc");
		goto freex;
	}
	memcpy(con->buf, buf, n);
	con->buflen = n;
       	printf("n=%zd buf=%s", n, buf);
	return;

freex:
	if (con->buf != NULL)
                free(con->buf);
	con->buf = NULL;
	con->buflen = 0;
	return;
}

/*
 * Writes data available in the context to standard output.
 */
static void
writestdout(concontxt *con)
{
	write(con->lfd, con->buf, con->buflen);

	if (con->buf != NULL)
                free(con->buf);
	con->buf = NULL;
	con->buflen = 0;
	return;
}

static void
myaccept(concontxt *con)
{
	int r;
	char host[NI_MAXHOST];
	int newfd;
	struct sockaddr_storage sa;
	socklen_t salen = sizeof (sa);

	newfd = accept(con->confd, (struct sockaddr *)&sa, &salen);
	printf("newfd=%d\n", newfd);
	if (newfd < 0)
		err(1, "accept");
	con->clifd = newfd;

	/* resolv */
	r = getnameinfo((struct sockaddr *)&sa, salen,
	    host, sizeof (host), NULL, 0, 0);
	if (r < 0) {
		warn("getnameinfo");
		snprintf(host, sizeof (host), "unknown");
	}
	printf("host=%s\n", host);
	return;
}

static void
myconnect(concontxt *con)
{
	int r;

	r = connect(con->confd,
	    con->clinfo->ai_addr,
	    con->clinfo->ai_addrlen);
	if (r < 0)
		warn("connect");
	con->clifd = con->confd;
	con->confd = -1;
	return;
}

int 
main(int argc, char *argv[])
{
	int c;
	int highsock, readsocks;
	struct timeval timeout;
	char *host, *port;
	concontxt *con;

	while ((c = getopt(argc, argv, "ht")) != -1) {
		switch (c) {
		case 'h':
			usage(1);
			break;
		case 't':
			tflag = 1;
			break;
		default:
			usage(1);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 2)
		usage(1);
	host = argv[0];
	port = argv[1];

	con = (concontxt *) malloc(sizeof (concontxt));
	if (con == NULL)
		err(1, "malloc");
	/* defaults */
	con->wfd = fileno(stdin);
	con->lfd = fileno(stdout);
	con->contype = (tflag || sflag) ? TCPCON : UDPCON;
	con->clifd = -1;

	prepare_socket(con, host, port, 1);

	timeout.tv_sec = 1;
	timeout.tv_usec = 0; 
	/* file descriptors for reading are stdin and confd */
	while (1) {
		FD_ZERO(&(con->lset));
		FD_SET(con->wfd, &(con->lset));
		highsock = con->wfd;
		if (con->confd != -1) {
			FD_SET(con->confd, &(con->lset));
			highsock = con->confd;
		}
		if (con->clifd != -1) {
			FD_SET(con->clifd, &(con->lset));
			highsock = con->clifd;
		}
		if (qflag)
			goto freex;
		readsocks = select(highsock + 1, &(con->lset), (fd_set *)0, 
		  (fd_set *)0, &timeout);
		if (readsocks < 0) {
			warn("select");
			goto freex;
		}
		if (readsocks == 0) {
			/* show that we are alive */
			printf(".");
			fflush(stdout);
		} else {
			if (FD_ISSET(con->wfd, &(con->lset))) {
				if (con->clifd == -1
				    && con->contype != UDPCON) {
					close(con->confd);
					prepare_socket(con, host, port, 0);
					myconnect(con);
				}
				readstdin(con);
				writesock(con);
			}
			if (con->confd != -1 &&
			    FD_ISSET(con->confd, &(con->lset))) {
				if (con->contype == UDPCON) {
					readsock(con);
					writestdout(con);
				} else {
					myaccept(con);
				}
			}
			if (con->clifd != -1 &&
			    FD_ISSET(con->clifd, &(con->lset))) {
				readsock(con);
				writestdout(con);
			}
		}
	}

freex:
	printf("exiting\n");
	if (con != NULL) {
		close(con->confd);
		close(con->clifd);
		free(con);
	}
	return (0);
}
