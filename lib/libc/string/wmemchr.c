/*-
 * Copyright (c)1999 Citrus Project,
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
 * citrus Id: wmemchr.c,v 1.2 2000/12/20 14:08:31 itojun Exp
 * $NetBSD: wmemchr.c,v 1.1 2000/12/23 23:14:37 itojun Exp $
 * $FreeBSD: src/lib/libc/string/wmemchr.c,v 1.6 2002/09/21 00:29:23 tjr Exp $
 * $DragonFly: src/lib/libc/string/wmemchr.c,v 1.3 2005/04/28 13:25:12 joerg Exp $
 */

#include <sys/types.h>
#include <wchar.h>

wchar_t	*
wmemchr(const wchar_t *s, wchar_t c, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (*s == c) {
			/* LINTED const castaway */
			return(__DECONST(wchar_t *, s));
		}
		s++;
	}
	return (NULL);
}
