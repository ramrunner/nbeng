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

/*SSL flag*/
int fssl = 0;

/*connection context */
typedef struct concontxt_t {
	int clifd; /*client fd*/
	int srvfd; /*server fd*/
	struct addrinfo *clinfo;
	struct addrinfo *srvinfo;
} concontxt;

static void
usage(const char *s)
{
        fprintf(stderr,
                "usage: %s [OPTIONS] <remote-addr> <remote-port> <local-port>\n", s);
        fprintf(stderr, " -s\tSSL on\n");
        fprintf(stderr, " -h\tThis help screen\n");
}

/*connect to in port port and store the fds in the context*/
static void
connectit(concontxt *c, char *to, char *port){
	struct addrinfo cli_hints, *cli_servinfo, *p0;
	int rv, cli_sockfd;
	if( (c == NULL) || (to == NULL) || (port == NULL))
		errx(1, "conectit was passed a null arg");
	memset(&cli_hints, 0, sizeof(cli_hints));
	cli_hints.ai_family = AF_INET;
	cli_hints.ai_socktype = SOCK_DGRAM;
	rv = getaddrinfo(to, port, &cli_hints, &cli_servinfo);
	if (rv)
		errx(1, "getaddrinfo: %s", gai_strerror(rv));

	for (p0 = cli_servinfo; p0; p0 = p0->ai_next) {
		cli_sockfd = socket(p0->ai_family, p0->ai_socktype,
				    p0->ai_protocol);
		if (cli_sockfd < 0)
			continue;
		break;
	}
	if (!p0)
		errx(1, "failed to bind socket");
	/*all was ok , so we register the socket to the context*/
	c->clifd = cli_sockfd;
	c->clinfo = cli_servinfo;
}

int 
main(int argc,char *argv[])
{
	/*our input is the stdin for now, we record lines and send them*/
	int recfd = STDIN_FILENO;
	char *prog = *argv;
	int c;
	fd_set rsocks;
	int highsock, readsocks;
	struct timeval timeout;
	char *inbuf=NULL, *linbuf=NULL;
	ssize_t inbufln;
	concontxt *con = (concontxt *) malloc(sizeof(concontxt));
	while ((c = getopt(argc, argv, "hs")) != -1) {
		switch (c) {
		case 'h':
			usage(prog);
			goto freex;
			break;
		case 's':
			fssl = 1;
			break;
		case '?':
		default:
			goto freex;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 3) {
		usage(prog);
		goto freex;
	}
	/*connect to the host*/
	connectit(con, argv[0], argv[1]);
	/*XXX: this should change when we add the srv fd*/
	highsock = recfd;
	/*these are the sockets for reading (stdin + server fd)*/
	FD_ZERO(&rsocks);
	FD_SET(recfd, &rsocks);
	while (1) {
		FD_ZERO(&rsocks);
		FD_SET(recfd, &rsocks);
		timeout.tv_sec = 1;
		timeout.tv_usec = 0;
		readsocks = select(highsock + 1, &rsocks, (fd_set *) 0, (fd_set *) 0, &timeout);
		if(readsocks < 0)
			goto freex;
		if (readsocks == 0) {
			/*show that we are alive*/
			printf(".");
			fflush(stdout);
		} else {
			if (FD_ISSET(recfd, &rsocks)){
				/*deal with input data*/
				inbuf = fgetln(stdin, &inbufln);
				if (inbuf[inbufln - 1] == '\n')
                             		inbuf[inbufln - 1] = '\0';
                     		else {
                             		/* EOF without EOL, copy and add the NUL */
                             		if ((linbuf = malloc(inbufln + 1)) == NULL){
						warnx("linbuf alloc failed");
						goto freex;
					}
                             		memcpy(linbuf, inbuf, inbufln);
                             		linbuf[inbufln] = '\0';
                             		inbuf = linbuf;
				}
				if ((strncmp(inbuf, "Q", 2) == 0))
					goto freex;
				printf("got msg: %s , sending it \n", inbuf);
				sendto(con->clifd, inbuf, inbufln, 0, con->clinfo->ai_addr, con->clinfo->ai_addrlen);
				fflush(stdout);
				/*free the buf*/
				if(linbuf != NULL)
					free(linbuf);
			}
		}
	}

freex:	if (con != NULL){
		close(con->clifd);
		free(con);
	}
	if (inbuf != NULL)
		free(inbuf);
	return 0;
}
