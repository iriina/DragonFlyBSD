/*-
 * Copyright (c) 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Christos Zoulas.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * @(#)for.c	8.1 (Berkeley) 6/6/93
 * $FreeBSD: src/usr.bin/make/for.c,v 1.35 2005/02/10 14:39:05 harti Exp $
 * $DragonFly: src/usr.bin/make/for.c,v 1.47 2005/09/24 07:38:03 okumoto Exp $
 */

/*-
 * for.c --
 *	Functions to handle loops in a makefile.
 *
 * Interface:
 *	For_Eval	Evaluate the loop in the passed line.
 *	For_Run		Run accumulated loop
 *
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "dir.h"
#include "for.h"
#include "globals.h"
#include "lst.h"
#include "make.h"
#include "parse.h"
#include "util.h"
#include "var.h"

/*
 * For statements are of the form:
 *
 * .for <variable> in <varlist>
 * ...
 * .endfor
 *
 * The trick is to look for the matching end inside for for loop
 * To do that, we count the current nesting level of the for loops.
 * and the .endfor statements, accumulating all the statements between
 * the initial .for loop and the matching .endfor;
 * then we evaluate the for loop for each variable in the varlist.
 */

static int	forLevel = 0;	/* Nesting level */
static char	*forVar;	/* Iteration variable */
static Buffer	*forBuf;	/* Commands in loop */
static Lst	forLst;		/* List of items */

/**
 * For_For
 *	Evaluate the for loop in the passed line. The line
 *	looks like this:
 *	    .for <variable> in <varlist>
 *	The line pointer points just behind the for.
 *
 * Results:
 *	true: Syntax ok.
 *	false: Syntax error.
 */
bool
For_For(char *line)
{
	char	*ptr;
	char	*wrd;
	char	*sub;
	Buffer	*buf;

	ptr = line;

	/*
	 * Skip space between for and the variable.
	 */
	for (ptr++; *ptr && isspace((u_char)*ptr); ptr++)
		;

	/*
	 * Grab the variable
	 */
	for (wrd = ptr; *ptr && !isspace((u_char)*ptr); ptr++)
		;

	buf = Buf_Init(0);
	Buf_AppendRange(buf, wrd, ptr);
	forVar = Buf_Data(buf);

	if (Buf_Size(buf) == 0) {
		Buf_Destroy(buf, true);
		Parse_Error(PARSE_FATAL, "missing variable in for");
		return (false);
	}
	Buf_Destroy(buf, false);

	/*
	 * Skip to 'in'.
	 */
	while (*ptr && isspace((u_char)*ptr))
		ptr++;

	/*
	 * Grab the `in'
	 */
	if (ptr[0] != 'i' || ptr[1] != 'n' || !isspace((u_char)ptr[2])) {
		free(forVar);
		Parse_Error(PARSE_FATAL, "missing `in' in for");
		fprintf(stderr, "%s\n", ptr);
		return (false);
	}
	ptr += 3;

	/*
	 * Skip to values
	 */
	while (*ptr && isspace((u_char)*ptr))
		ptr++;

	/*
	 * Make a list with the remaining words
	 */
	sub = Buf_Peel(Var_Subst(ptr, VAR_CMD, false));
	for (ptr = sub; *ptr != '\0' && isspace((u_char)*ptr); ptr++)
		;

	Lst_Init(&forLst);
	buf = Buf_Init(0);
	for (wrd = ptr; *ptr != '\0'; ptr++) {
		if (isspace((u_char)*ptr)) {
			Buf_AppendRange(buf, wrd, ptr);
			Lst_AtFront(&forLst, Buf_Peel(buf));

			buf = Buf_Init(0);
			while (*ptr != '\0' && isspace((u_char)*ptr))
				ptr++;
			wrd = ptr--;
		}
	}
	DEBUGF(FOR, ("For: Iterator %s List %s\n", forVar, sub));

	if (ptr - wrd > 0) {
		Buf_AppendRange(buf, wrd, ptr);
		Lst_AtFront(&forLst, Buf_Peel(buf));
	} else {
		Buf_Destroy(buf, true);
	}
	free(sub);

	forBuf = Buf_Init(0);
	forLevel++;
	return (true);
}

/**
 * For_Eval
 *	Eat a line of the .for body looking for embedded .for loops
 *	and the .endfor
 */
bool
For_Eval(char *line)
{
	char *ptr;

	ptr = line;

	if (*ptr == '.') {
		/*
		 * Need to check for 'endfor' and 'for' to find the end
		 * of our loop or to find embedded for loops.
		 */
		for (ptr++; *ptr != '\0' && isspace((u_char)*ptr); ptr++)
			;

		/* XXX the isspace is wrong */
		if (strncmp(ptr, "endfor", 6) == 0 &&
		    (isspace((u_char)ptr[6]) || ptr[6] == '\0')) {
			DEBUGF(FOR, ("For: end for %d\n", forLevel));
			if (forLevel == 0) {
				/* should not be here */
				abort();
			}
			forLevel--;

		} else if (strncmp(ptr, "for", 3) == 0 &&
		    isspace((u_char)ptr[3])) {
			forLevel++;
			DEBUGF(FOR, ("For: new loop %d\n", forLevel));
		}
	}

	if (forLevel != 0) {
		/*
		 * Still in loop - append the line
		 */
		Buf_Append(forBuf, line);
		Buf_AddByte(forBuf, '\n');
		return (true);
	}

	return (false);
}

/*-
 *-----------------------------------------------------------------------
 * For_Run --
 *	Run the for loop, immitating the actions of an include file
 *
 * Results:
 *	None.
 *
 * Side Effects:
 *	The values of the variables forLst, forVar and forBuf are freed.
 *
 *-----------------------------------------------------------------------
 */
void
For_Run(int lineno)
{
	Lst		values;	/* list of values for the variable */
	char		*var;	/* the variable's name */
	Buffer		*buf;	/* the contents of the for loop */
	const char	*val;	/* current value of loop variable */
	LstNode		*ln;
	char		*str;

	if (forVar == NULL || forBuf == NULL)
		return;

	/* copy the global variables to have them free for embedded fors */
	var = forVar;
	buf = forBuf;
	Lst_Init(&values);
	Lst_Concat(&values, &forLst, LST_CONCLINK);

	forVar = NULL;
	forBuf = NULL;

	LST_FOREACH(ln, &values) {
		val = Lst_Datum(ln);
		Var_SetGlobal(var, val);

		DEBUGF(FOR, ("--- %s = %s\n", var, val));
		str = Buf_Peel(Var_SubstOnly(var, Buf_Data(buf), false));

		Parse_FromString(str, lineno);
		Var_Delete(var, VAR_GLOBAL);
	}

	free(var);
	Lst_Destroy(&values, free);
	Buf_Destroy(buf, true);
}
