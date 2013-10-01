#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "util.h"
#include "list.h"


/*
 * connect to lirc on the localhost
 *
 * listen on the proxy port
 * 
 * select on:
 * 	* the listenning socket for new client connections
 * 		* add it to a list of connections
 * 	* established proxy sockets for new commands
 * 		* send it to lirc
 * 		* read from lirc and write it to the proxy socket
 * 		until "END"
 */


struct connection {
	struct list_head list;
	int socket;
};

void pferror(const int errsv, const char* format, ...);


int handle_one_command(int lirc, int client)
{
	static char cmdbuf[4096];
	char *head = cmdbuf, *tail = cmdbuf, *end = cmdbuf +
		sizeof(cmdbuf), *next;
	static char replybuf[4096];
	char *rhead, *rtail, *rend = replybuf + sizeof(replybuf);
	int result = 0, retval;

	while (true) {
		/*
		 * cmdbuf
		 * head: command start
		 * next: command end
		 * tail: recv buf end
		 * end: buffer end
		 */
		/* read until there is at least one complete line */
		do {
			if (tail == end) {
				fprintf(stderr, "Warning: client request too long\n");
				return 0;
			}

			retval = recv(client, tail, end - tail, 0);
			if (retval == -1)
			{
				pferror(errno, "line %d", __LINE__);
				abort();
			} else if (retval == 0) {
				if (tail != head) {
					fprintf(stderr, "Warning: incomplete client request\n");
					return 0;
				} else {
					return result;
				}
			} else {
				tail += retval;
			}
		} while ((next = memchr(head, '\n', tail - head)) == NULL);
		next++;
		result++;

		/* send it to lirc */
		do {
			retval = send(lirc, head, next - head, 0);
			if (retval == -1)
			{
				pferror(errno, "line %d", __LINE__);
				abort();
			} else {
				head += retval;
			}

		} while (head < next);

		/* read lirc's reply (until a line that says "END") */
		/*
		 * replybuf
		 * rhead: send buf start
		 * rtail: reply end
		 * rend: buffer end
		 */
		rhead = rtail = replybuf;
		do {
			int i;
			const char token[] = "END\n";

			if (rtail == rend) {
				fprintf(stderr, "Warning: server response too long\n");
				return 0;
			}

			retval = recv(lirc, rtail, rend - rtail, 0);
			if (retval == -1)
			{
				pferror(errno, "line %d", __LINE__);
				abort();
			} else if (retval == 0) {
				fprintf(stderr, "Error: connection lost with the server\n");
				abort();
			} else {
				rtail += retval;
			}

			retval = 0;
			for (i = 1; i <= min((unsigned long) (rtail - rhead), strlen(token)); i++) {
				if (*(rtail - i) != *(token + strlen(token) - i)) {
					retval = 1;
					break;
				}
			}
			if (retval == 0 &&
			    !(rtail - strlen(token) == rhead ||
			      *(rtail - strlen(token) - 1) == '\n')) {
				retval = 1;
			}
		} while (retval == 1);

		/* send it to the client */
		do {
			retval = send(client, rhead, rtail - rhead, 0);
			if (retval == -1)
			{
				pferror(errno, "line %d", __LINE__);
				abort();
			} else {
				rhead += retval;
			}

		} while (rhead < rtail);
	}
}


int main(int argc, char *argv[])
{
	int lirc, proxy;
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(8764),
		.sin_addr = {
			.s_addr = htonl(INADDR_LOOPBACK),
		},
	};
	LIST_HEAD(conn_head);
	int retval;

	lirc = socket(AF_INET, SOCK_STREAM, 0);
	printf("Connecting to local lirc... ");
	retval = connect(lirc, (struct sockaddr *) &addr, sizeof(addr));
	if (retval == -1)
	{
		pferror(errno, "line %d", __LINE__);
		abort();
	}
	printf("ok!\n");

	addr.sin_port = htons(8765);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	proxy = socket(AF_INET, SOCK_STREAM, 0);
	retval = bind(proxy, (struct sockaddr *) &addr, sizeof(addr));
	if (retval == -1)
	{
		pferror(errno, "line %d", __LINE__);
		abort();
	}
	retval = listen(proxy, 1);
	if (retval == -1)
	{
		pferror(errno, "line %d", __LINE__);
		abort();
	}
	retval = fcntl(proxy, F_SETFL, O_NONBLOCK);
	if (retval == -1)
	{
		pferror(errno, "line %d", __LINE__);
		abort();
	}
	printf("Listenning on proxy port\n");

	while (true) {
		fd_set set;
		int nfds;
		struct connection *conn, *next;

		FD_ZERO(&set);
		FD_SET(proxy, &set);
		nfds = proxy;

		list_for_each_entry(conn, &conn_head, list) {
			FD_SET(conn->socket, &set);
			nfds = max(nfds, conn->socket);
		}

		retval = select(nfds + 1, &set, NULL, NULL, NULL);
		if (retval == -1)
		{
			pferror(errno, "line %d", __LINE__);
			abort();
		} else if (retval == 0) {
			continue;
		}

		if (FD_ISSET(proxy, &set)) {
			retval = accept(proxy, NULL, NULL);
			if (retval == -1)
			{
				pferror(errno, "line %d", __LINE__);
				abort();
			}
			conn = malloc(sizeof(*conn));
			conn->socket = retval;
			list_add(&conn->list, &conn_head);
			printf("New connection\n");
		}

		list_for_each_entry_safe(conn, next, &conn_head, list) {
			if (FD_ISSET(conn->socket, &set)) {
				retval = handle_one_command(lirc, conn->socket);
				if (retval == 0) {
					printf("Connection closed\n");
					close(conn->socket);
					list_del(&conn->list);
					free(conn);
				}
			}
		}
	}

	return EXIT_SUCCESS;
}


/*
 * Print a custom error message followed by the message associated with an
 * errno.
 *
 * Mostly copied from printf(3)
 *
 * Args:
 *   errsv:        the errno for which to print the message
 *   format:       a printf-style format string
 *   ...:          the arguments associated with the format
 */
void pferror(const int errsv, const char* format, ...)
{
	/* Guess we need no more than 100 bytes. */
	int n, size= 100;
	char *p, *np;
	va_list ap;

	if ((p= malloc(size)) == NULL)
	{
		return;
	}

	while (true)
	{
		/* Try to print in the allocated space. */
		va_start(ap, format);
		n= vsnprintf(p, size, format, ap);
		va_end(ap);
		/* If that worked, output the string. */
		if (n > -1 && n < size)
		{
			fputs(p, stderr);
			fprintf(stderr, ": %s\n", strerror(errsv));
			return;
		}

		/* Else try again with more space. */
		if (n > -1)    /* glibc 2.1 */
		{
			size= n + 1; /* precisely what is needed */
		}
		else           /* glibc 2.0 */
		{
			size*= 2;  /* twice the old size */
		}

		if ((np= realloc(p, size)) == NULL)
		{
			free(p);
			return;
		}
		else
		{
			p= np;
		}
	}
}
