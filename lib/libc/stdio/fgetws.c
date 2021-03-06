/* $NetBSD: fgetws.c,v 1.2 2006/07/03 17:06:36 tnozaki Exp $ */
/* $DragonFly: src/lib/libc/stdio/fgetws.c,v 1.1 2005/07/25 00:37:41 joerg Exp $ */

/*-
 * Copyright (c) 2002 Tim J. Robbins.
 * All rights reserved.
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
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Original version ID:
 * FreeBSD: src/lib/libc/stdio/fgetws.c,v 1.4 2002/09/20 13:25:40 tjr Exp
 *
 */

#include "namespace.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <wchar.h>
#include "un-namespace.h"

#include "libc_private.h"
#include "local.h"
#include "priv_stdio.h"

wchar_t *
fgetws(wchar_t * __restrict ws, int n, FILE * __restrict fp)
{
	wchar_t *wsp;
	wint_t wc;

	_DIAGASSERT(fp != NULL);
	_DIAGASSERT(ws != NULL);

	FLOCKFILE(fp);
	_SET_ORIENTATION(fp, 1);

	if (n <= 0) {
		errno = EINVAL;
		goto error;
	}

	wsp = ws;
	while (n-- > 1) {
		wc = __fgetwc_unlock(fp);
		if (__sferror(fp) != 0)
			goto error;
		if (__sfeof(fp) != 0) {
			if (wsp == ws) {
				/* EOF/error, no characters read yet. */
				goto error;
			}
			break;
		}
		*wsp++ = (wchar_t)wc;
		if (wc == L'\n') {
			break;
		}
	}

	*wsp++ = L'\0';
	FUNLOCKFILE(fp);

	return (ws);

error:
	FUNLOCKFILE(fp);
	return (NULL);
}
