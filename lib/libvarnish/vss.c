/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2010 Varnish Software AS
 * All rights reserved.
 *
 * Author: Dag-Erling Smørgrav <des@des.no>
 * Author: Cecilie Fritzvold <cecilihf@linpro.no>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "config.h"

#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vas.h"
#include "vsa.h"
#include "vss.h"
#include "vtcp.h"

/* lightweight addrinfo */
struct vss_addr {
	int			 va_family;
	int			 va_socktype;
	int			 va_protocol;
	socklen_t		 va_addrlen;
	struct sockaddr_storage	 va_addr;
};

/*lint -esym(754, _storage) not ref */

/*
 * Take a string provided by the user and break it up into address and
 * port parts.  Examples of acceptable input include:
 *
 * "localhost" - "localhost:80"
 * "127.0.0.1" - "127.0.0.1:80"
 * "0.0.0.0" - "0.0.0.0:80"
 * "[::1]" - "[::1]:80"
 * "[::]" - "[::]:80"
 *
 * See also RFC5952
 */

static const char *
vss_parse(char *str, char **addr, char **port)
{
	char *p;

	*addr = *port = NULL;

	if (str[0] == '[') {
		/* IPv6 address of the form [::1]:80 */
		*addr = str + 1;
		p = strchr(str, ']');
		if (p == NULL)
			return ("IPv6 address lacks ']'");
		*p++ = '\0';
		if (*p == '\0')
			return (NULL);
		if (*p != ' ' && *p != ':')
			return ("IPv6 address has wrong port separator");
	} else {
		/* IPv4 address of the form 127.0.0.1:80, or non-numeric */
		*addr = str;
		p = strchr(str, ' ');
		if (p == NULL)
			p = strchr(str, ':');
		if (p == NULL)
			return (NULL);
		if (p == str)
			*addr = NULL;
	}
	*p++ = '\0';
	*port = p;
	return (NULL);
}

/*
 * Look up an address, using a default port if provided, and call
 * the callback function with the suckaddrs we find.
 * If the callback function returns anything but zero, we terminate
 * and pass that value.
 */

int
VSS_resolver(const char *addr, const char *def_port, vss_resolved_f *func,
    void *priv, const char **err)
{
	struct addrinfo hints, *res0, *res;
	struct suckaddr *vsa;
	char *h;
	char *adp, *hop;
	int ret;

	*err = NULL;
	h = strdup(addr);
	AN(h);
	*err = vss_parse(h, &hop, &adp);
	if (*err != NULL) {
		free(h);
		return (-1);
	}
	if (adp != NULL)
		def_port = adp;

	memset(&hints, 0, sizeof hints);
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	ret = getaddrinfo(hop, def_port, &hints, &res0);
	free(h);
	if (ret != 0) {
		*err = gai_strerror(ret);
		return (-1);
	}
	for (res = res0; res != NULL; res = res->ai_next) {
		vsa = VSA_Malloc(res->ai_addr, res->ai_addrlen);
		if (vsa != NULL) {
			ret = func(priv, vsa);
			free(vsa);
			if (ret)
				break;
		}
	}
	freeaddrinfo(res0);
	return (ret);
}

/*
 * For a given host and port, return a list of struct vss_addr, which
 * contains all the information necessary to open and bind a socket.  One
 * vss_addr is returned for each distinct address returned by
 * getaddrinfo().
 *
 * The value pointed to by the tap parameter receives a pointer to an
 * array of pointers to struct vss_addr.  The caller is responsible for
 * freeing each individual struct vss_addr as well as the array.
 *
 * The return value is the number of addresses resoved, or zero.
 *
 * If the addr argument contains a port specification, that takes
 * precedence over the port argument.
 *
 * XXX: We need a function to free the allocated addresses.
 */
int
VSS_resolve(const char *addr, const char *port, struct vss_addr ***vap)
{
	struct addrinfo hints, *res0, *res;
	struct vss_addr **va;
	int i, ret;
	char *h;
	char *adp, *hop;

	*vap = NULL;
	memset(&hints, 0, sizeof hints);
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	h = strdup(addr);
	AN(h);
	if (vss_parse(h, &hop, &adp) != NULL) {
		free(h);
		return (0);
	}

	ret = getaddrinfo(hop, adp == NULL ? port : adp, &hints, &res0);
	free(h);

	if (ret != 0)
		return (0);

	XXXAN(res0);
	for (res = res0, i = 0; res != NULL; res = res->ai_next)
		i++;
	if (i == 0) {
		freeaddrinfo(res0);
		return (0);
	}
	va = calloc(i, sizeof *va);
	XXXAN(va);
	*vap = va;
	for (res = res0, i = 0; res != NULL; res = res->ai_next, ++i) {
		va[i] = calloc(1, sizeof(**va));
		XXXAN(va[i]);
		va[i]->va_family = res->ai_family;
		va[i]->va_socktype = res->ai_socktype;
		va[i]->va_protocol = res->ai_protocol;
		va[i]->va_addrlen = res->ai_addrlen;
		xxxassert(va[i]->va_addrlen <= sizeof va[i]->va_addr);
		memcpy(&va[i]->va_addr, res->ai_addr, va[i]->va_addrlen);
	}
	freeaddrinfo(res0);
	return (i);
}

/*
 * Given a struct vss_addr, open a socket of the appropriate type, and bind
 * it to the requested address.
 *
 * If the address is an IPv6 address, the IPV6_V6ONLY option is set to
 * avoid conflicts between INADDR_ANY and IN6ADDR_ANY.
 */

int
VSS_bind(const struct vss_addr *va)
{
	int sd, val;

	sd = socket(va->va_family, va->va_socktype, va->va_protocol);
	if (sd < 0) {
		perror("socket()");
		return (-1);
	}
	val = 1;
	if (setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof val) != 0) {
		perror("setsockopt(SO_REUSEADDR, 1)");
		(void)close(sd);
		return (-1);
	}
#ifdef IPV6_V6ONLY
	/* forcibly use separate sockets for IPv4 and IPv6 */
	val = 1;
	if (va->va_family == AF_INET6 &&
	    setsockopt(sd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof val) != 0) {
		perror("setsockopt(IPV6_V6ONLY, 1)");
		(void)close(sd);
		return (-1);
	}
#endif
	if (bind(sd, (const void*)&va->va_addr, va->va_addrlen) != 0) {
		perror("bind()");
		(void)close(sd);
		return (-1);
	}
	return (sd);
}

/*
 * Given a struct vss_addr, open a socket of the appropriate type, bind it
 * to the requested address, and start listening.
 *
 * If the address is an IPv6 address, the IPV6_V6ONLY option is set to
 * avoid conflicts between INADDR_ANY and IN6ADDR_ANY.
 */
int
VSS_listen(const struct vss_addr *va, int depth)
{
	int sd;

	sd = VSS_bind(va);
	if (sd >= 0)  {
		if (listen(sd, depth) != 0) {
			perror("listen()");
			(void)close(sd);
			return (-1);
		}
	}
	return (sd);
}
