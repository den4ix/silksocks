#include "common.h"

#ifndef SPLICE_F_MOVE
#define SPLICE_F_MOVE           0x01
#endif
#ifndef SPLICE_F_NONBLOCK
#define SPLICE_F_NONBLOCK       0x02
#endif
#ifndef SPLICE_F_MORE
#define SPLICE_F_MORE           0x04
#endif
#ifndef SPLICE_F_GIFT
#define SPLICE_F_GIFT           0x08
#endif

#define PRINT_PEERS(clientfd, proxyfd) \
    do { \
        int n = 0; \
        char buf[256]; \
        if (!peer2logbuf(clientfd, CLIENT)) { \
            SILK_LOG_ERRNO(ERR, "getpeername error on %s fd (%d)", "client", clientfd); \
            return 1; \
        } \
        n = snprintf(buf, sizeof(buf), "%s -- ", log_buf); \
        if (!peer2logbuf(proxyfd, SERVER)) { \
            SILK_LOG_ERRNO(ERR, "getpeername error on %s fd (%d)", "server", proxyfd); \
            return 1; \
        } \
        strncat(buf+n, log_buf, sizeof(buf)-n); \
        SILK_LOG(INFO, "%s", buf); \
    } while (0)

#ifdef USE_SELECT_NEGOTIATE
int negotiate(int clientfd, int proxyfd)
{
	int			maxfdp1, clienteof;
	ssize_t		n, nwritten;
	fd_set		rset, wset;
	char		to[BUFSIZE], fr[BUFSIZE];
	char		*toiptr, *tooptr, *friptr, *froptr;

#ifdef DEBUG
    PRINT_PEERS(clientfd, proxyfd);
#endif

    if (setnonblock(clientfd) == -1)
        return 1;
    if (setnonblock(proxyfd) == -1)
        return 1;

	toiptr = tooptr = to;
	friptr = froptr = fr;
	clienteof = 0;

	maxfdp1 = max(clientfd, proxyfd) + 1;
	for ( ; ; ) {
		FD_ZERO(&rset);
		FD_ZERO(&wset);
		if (clienteof == 0 && toiptr < &to[BUFSIZE])
			FD_SET(clientfd, &rset);	        /* read from client socket */
		if (friptr < &fr[BUFSIZE])
			FD_SET(proxyfd, &rset);			/* read from proxy server socket */
		if (tooptr != toiptr)
			FD_SET(proxyfd, &wset);			/* data to write to proxy server socket */
		if (froptr != friptr)
			FD_SET(clientfd, &wset);	    /* data to write to client socket */

		if (select(maxfdp1, &rset, &wset, NULL, NULL) == -1)
            return 1;

		if (FD_ISSET(clientfd, &rset)) {
			if ( (n = read(clientfd, toiptr, &to[BUFSIZE] - toiptr)) < 0) {
				if (errno != EAGAIN && errno != EWOULDBLOCK) {
					SILK_LOG_ERRNO(ERR, "(%d,%d): read error on clientfd", clientfd, proxyfd);
                    return 1;
                }

			} else if (n == 0) {
				SILK_LOG(INFO, "(%d,%d): EOF on clientfd", clientfd, proxyfd);

				clienteof = 1;			/* all done with client */
				if (tooptr == toiptr)
				    shutdown(proxyfd, SHUT_WR); /* send FIN */

			} else {
				SILK_LOG(INFO, "(%d,%d): read %ld bytes from clientfd", clientfd, proxyfd, n);

				toiptr += n;			/* # just read */
				FD_SET(proxyfd, &wset);	/* try and write to socket below */
			}
		}

		if (FD_ISSET(proxyfd, &rset)) {
			if ( (n = read(proxyfd, friptr, &fr[BUFSIZE] - friptr)) < 0) {
				if (errno != EAGAIN && errno != EWOULDBLOCK) {
					(2, "(%d,%d): read error on proxyfd", clientfd, proxyfd);
                    return 1;
                }

			} else if (n == 0) {
				SILK_LOG(INFO, "(%d,%d): EOF on proxy socket", clientfd, proxyfd);

				if (clienteof)
					return 0;		/* normal termination */
				else {
					SILK_LOG(INFO, "(%d,%d): proxy server terminated prematurely", clientfd, proxyfd);
                    return 1;
                }

			} else {
				SILK_LOG(INFO, "(%d,%d): read %ld bytes from proxy server socket", clientfd, proxyfd, n);

				friptr += n;		/* # just read */
				FD_SET(clientfd, &wset);	/* try and write below */
			}
		}

		if (FD_ISSET(clientfd, &wset) && ( (n = friptr - froptr) > 0)) {
			if ( (nwritten = write(clientfd, froptr, n)) < 0) {
				if (errno != EAGAIN && errno != EWOULDBLOCK) {
					SILK_LOG_ERRNO(INFO, "(%d,%d): write error to clientfd", clientfd, proxyfd);
                    return 1;
                }

			} else {
				SILK_LOG(INFO, "(%d,%d): wrote %ld bytes to client socket", clientfd, proxyfd, nwritten);

				froptr += nwritten;		/* # just written */
				if (froptr == friptr)
					froptr = friptr = fr;	/* back to beginning of buffer */
			}
		}

		if (FD_ISSET(proxyfd, &wset) && ( (n = toiptr - tooptr) > 0)) {
			if ( (nwritten = write(proxyfd, tooptr, n)) < 0) {
				if (errno != EAGAIN && errno != EWOULDBLOCK) {
					SILK_LOG_ERRNO(INFO, "(%d,%d): write error to proxyfd", clientfd, proxyfd);
                    return 1;
                }

			} else {
				SILK_LOG(INFO, "(%d,%d): wrote %ld bytes to proxy socket", clientfd, proxyfd, nwritten);

				tooptr += nwritten;	/* # just written */
				if (tooptr == toiptr) {
					toiptr = tooptr = to;	/* back to beginning of buffer */
					if (clienteof)
						shutdown(proxyfd, SHUT_WR);	/* send FIN */
				}
			}
		}
	}
}
#elif USE_SPLICE_NEGOTIATE
int negotiate(int clientfd, int proxyfd)
{
	int			maxfdp1, clienteof;
	ssize_t		n, nwritten;
	struct pollfd pfds[2];
    int pipesrv[2], pipecli[2];
    int npipecli, npipesrv;
    int timeout = timeo[CONNECT_L] * 1000;
    int status = 0;

#ifdef DEBUG
    PRINT_PEERS(clientfd, proxyfd);
#endif

    if (setnonblock(clientfd) == -1)
        return 1;
    if (setnonblock(proxyfd) == -1)
        return 1;

    if (pipe2(pipesrv, O_NONBLOCK) == -1)
        return 1;
    if (pipe2(pipecli, O_NONBLOCK) == -1) {
        close(pipesrv[0]);
        close(pipesrv[1]);
        return 1;
    }

    npipecli = npipesrv = 0;
	clienteof = 0;

    pfds[0].fd = clientfd;
    pfds[1].fd = proxyfd;
	maxfdp1 = max(clientfd, proxyfd) + 1;

	for ( ; ; ) {
        pfds[0].events = pfds[1].events = 0;
		if (clienteof == 0 && npipecli < BUFSIZE)
			pfds[0].events |= POLLIN;	        /* read from client socket */
		if (npipesrv < BUFSIZE)
			pfds[1].events |= POLLIN;			/* read from proxy server socket */
		if (npipecli)
			pfds[1].events |= POLLOUT;			/* data to write to proxy server socket */
		if (npipesrv)
			pfds[0].events |= POLLOUT;	    /* data to write to client socket */

        n = poll(pfds, 2, timeout);
		if (n == -1) {
            if (errno != EAGAIN && errno != EINTR) {
                SILK_LOG_ERRNO(INFO, "(%d,%d): poll error", clientfd, proxyfd);
                status = 1;
                goto done;
            }
            if (errno == EINTR)
                usleep(SLEEPTIME);
            continue;
        }
        if (n == 0) {
            status = 6;
            goto done;
        }

		if (pfds[0].revents & POLLIN) {
			if ((n = splice(clientfd, NULL, pipecli[1], NULL, BUFSIZE, SPLICE_F_MOVE|SPLICE_F_NONBLOCK|SPLICE_F_MORE)) < 0) {
				if (errno != EAGAIN && errno != EINTR) {
					SILK_LOG_ERRNO(INFO, "(%d,%d): read error on clientfd", clientfd, proxyfd);
                    status = 1;
                    goto done;
                }
                if (errno == EINTR)
                    usleep(SLEEPTIME);
                continue;

			} else if (n == 0) {
				SILK_LOG(INFO, "(%d,%d): EOF on clientfd", clientfd, proxyfd);

				clienteof = 1;			/* all done with client */
				if (npipecli == 0)
				    shutdown(proxyfd, SHUT_WR); /* send FIN */

			} else {
				SILK_LOG(INFO, "(%d,%d): read %ld bytes from clientfd", clientfd, proxyfd, n);

				npipecli += n;			/* # just read */
				pfds[1].events |= POLLOUT;	/* try and write to socket below */
			}
		}

		if (pfds[1].revents & POLLIN) {
			if ((n = splice(proxyfd, NULL, pipesrv[1], NULL, BUFSIZE, SPLICE_F_MOVE|SPLICE_F_NONBLOCK|SPLICE_F_MORE)) < 0) {
				if (errno != EAGAIN && errno != EINTR) {
					SILK_LOG_ERRNO(INFO, "(%d,%d): read error on proxyfd", clientfd, proxyfd);
                    status = 1;
                    goto done;
                }
                if (errno == EINTR)
                    usleep(SLEEPTIME);
                continue;

			} else if (n == 0) {
				SILK_LOG(INFO, "(%d,%d): EOF on proxy socket", clientfd, proxyfd);

				if (clienteof)
					status = 0;		/* normal termination */
				else {
					SILK_LOG(INFO, "(%d,%d): proxy server terminated prematurely", clientfd, proxyfd);
                    status = 1;
                }
                goto done;

			} else {
				SILK_LOG(INFO, "(%d,%d): read %ld bytes from proxy server socket", clientfd, proxyfd, n);

				npipesrv += n;		/* # just read */
				pfds[0].events |= POLLOUT;	/* try and write below */
			}
		}

		if ((pfds[0].revents & POLLOUT) && (npipesrv > 0)) {
			if ((nwritten = splice(pipesrv[0], NULL, clientfd, NULL, BUFSIZE, SPLICE_F_MOVE|SPLICE_F_NONBLOCK|SPLICE_F_MORE)) < 0) {
				if (errno != EAGAIN && errno != EINTR) {
					SILK_LOG_ERRNO(INFO, "(%d,%d): write error to clientfd", clientfd, proxyfd);
                    status = 1;
                    goto done;
                }
                if (errno == EINTR)
                    usleep(SLEEPTIME);
                continue;

			} else {
				SILK_LOG(INFO, "(%d,%d): wrote %ld bytes to client socket", clientfd, proxyfd, nwritten);

				npipesrv -= nwritten;		/* # just written */
			}
		}

		if ((pfds[1].revents & POLLOUT) && (npipecli > 0)) {
			if ((nwritten = splice(pipecli[0], NULL, proxyfd, NULL, BUFSIZE, SPLICE_F_MOVE|SPLICE_F_NONBLOCK|SPLICE_F_MORE)) < 0) {
				if (errno != EAGAIN && errno != EINTR) {
					SILK_LOG_ERRNO(INFO, "(%d,%d): write error to proxyfd", clientfd, proxyfd);
                    status = 1;
                    goto done;
                }
                if (errno == EINTR)
                    usleep(SLEEPTIME);
                continue;

			} else {
				SILK_LOG(INFO, "(%d,%d): wrote %ld bytes to proxy socket", clientfd, proxyfd, nwritten);

				npipecli -= nwritten;	/* # just written */
                if (npipecli == 0 && clienteof)
                    shutdown(proxyfd, SHUT_WR);	/* send FIN */
			}
		}
	}
done:
    close(pipecli[0]);
    close(pipecli[1]);
    close(pipesrv[0]);
    close(pipesrv[1]);
    return status;
}
#else
int negotiate(int clientfd, int proxyfd)
{
	int			maxfdp1, clienteof;
	ssize_t		n, nwritten;
	struct pollfd pfds[2];
	char		to[BUFSIZE], fr[BUFSIZE];
	char		*toiptr, *tooptr, *friptr, *froptr;
    int timeout = timeo[CONNECT_L] * 1000;

//#ifdef DEBUG
    //PRINT_PEERS(clientfd, proxyfd);
//#endif

    if (setnonblock(clientfd) == -1)
        return 1;
    if (setnonblock(proxyfd) == -1)
        return 1;

	toiptr = tooptr = to;
	friptr = froptr = fr;
	clienteof = 0;

    pfds[0].fd = clientfd;
    pfds[1].fd = proxyfd;
	maxfdp1 = max(clientfd, proxyfd) + 1;
	for ( ; ; ) {
        pfds[0].events = pfds[1].events = 0;    
		if (clienteof == 0 && toiptr < &to[BUFSIZE])
			pfds[0].events |= POLLIN;	        /* read from client socket */
		if (friptr < &fr[BUFSIZE])
			pfds[1].events |= POLLIN;			/* read from proxy server socket */
		if (tooptr != toiptr)
			pfds[1].events |= POLLOUT;			/* data to write to proxy server socket */
		if (froptr != friptr)
			pfds[0].events |= POLLOUT;	    /* data to write to client socket */

        n = poll(pfds, 2, timeout);
		if (n == -1) {
            if (errno != EAGAIN && errno != EINTR) {
                SILK_LOG_ERRNO(INFO, "(%d,%d): poll error", clientfd, proxyfd);
                return 1;
            }
            if (errno == EINTR)
                usleep(SLEEPTIME);
            continue;
        }
        if (n == 0)
            return 6;

		if (pfds[0].revents & POLLIN) {
			if ((n = read(clientfd, toiptr, &to[BUFSIZE] - toiptr)) < 0) {
				if (errno != EAGAIN && errno != EINTR) {
					SILK_LOG_ERRNO(INFO, "(%d,%d): read error on clientfd", clientfd, proxyfd);
                    return 1;
                }
                if (errno == EINTR)
                    usleep(SLEEPTIME);
                continue;

			} else if (n == 0) {
				SILK_LOG(INFO, "(%d,%d): EOF on clientfd", clientfd, proxyfd);

				clienteof = 1;			/* all done with client */
				if (tooptr == toiptr)
				    shutdown(proxyfd, SHUT_WR); /* send FIN */

			} else {
				SILK_LOG(INFO, "(%d,%d): read %ld bytes from clientfd", clientfd, proxyfd, n);

				toiptr += n;			/* # just read */
				pfds[1].events |= POLLOUT;	/* try and write to socket below */
			}
		}

		if (pfds[1].revents & POLLIN) {
			if ((n = read(proxyfd, friptr, &fr[BUFSIZE] - friptr)) < 0) {
				if (errno != EAGAIN && errno != EINTR) {
					SILK_LOG_ERRNO(INFO, "(%d,%d): read error on proxyfd", clientfd, proxyfd);
                    return 1;
                }
                if (errno == EINTR)
                    usleep(SLEEPTIME);
                continue;

			} else if (n == 0) {
				SILK_LOG(INFO, "(%d,%d): EOF on proxy socket", clientfd, proxyfd);

				if (clienteof)
					return 0;		/* normal termination */
				else {
					SILK_LOG(INFO, "(%d,%d): proxy server terminated prematurely", clientfd, proxyfd);
                    return 1;
                }

			} else {
				SILK_LOG(INFO, "(%d,%d): read %ld bytes from proxy server socket", clientfd, proxyfd, n);

				friptr += n;		/* # just read */
				pfds[0].events |= POLLOUT;	/* try and write below */
			}
		}

		if ((pfds[0].revents & POLLOUT) && ((n = friptr - froptr) > 0)) {
			if ((nwritten = write(clientfd, froptr, n)) < 0) {
				if (errno != EAGAIN && errno != EINTR) {
					SILK_LOG_ERRNO(INFO, "(%d,%d): write error to clientfd", clientfd, proxyfd);
                    return 1;
                }
                if (errno == EINTR)
                    usleep(SLEEPTIME);
                continue;

			} else {
				SILK_LOG(INFO, "(%d,%d): wrote %ld bytes to client socket", clientfd, proxyfd, nwritten);

				froptr += nwritten;		/* # just written */
				if (froptr == friptr)
					froptr = friptr = fr;	/* back to beginning of buffer */
			}
		}

		if ((pfds[1].revents & POLLOUT) && ((n = toiptr - tooptr) > 0)) {
			if ((nwritten = write(proxyfd, tooptr, n)) < 0) {
				if (errno != EAGAIN && errno != EINTR) {
					SILK_LOG_ERRNO(INFO, "(%d,%d): write error to proxyfd", clientfd, proxyfd);
                    return 1;
                }
                if (errno == EINTR)
                    usleep(SLEEPTIME);
                continue;

			} else {
				SILK_LOG(INFO, "(%d,%d): wrote %ld bytes to proxy socket", clientfd, proxyfd, nwritten);

				tooptr += nwritten;	/* # just written */
				if (tooptr == toiptr) {
					toiptr = tooptr = to;	/* back to beginning of buffer */
					if (clienteof)
						shutdown(proxyfd, SHUT_WR);	/* send FIN */
				}
			}
		}
	}
}
#endif
