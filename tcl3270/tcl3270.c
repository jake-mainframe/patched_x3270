/*
 * Copyright (c) 1993-2009, 2013-2018 Paul Mattes.
 * Copyright (c) 1990, Jeff Sparkes.
 * Copyright (c) 1989, Georgia Tech Research Corporation (GTRC), Atlanta,
 *  GA 30332.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the names of Paul Mattes, Jeff Sparkes, GTRC nor their
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY PAUL MATTES, JEFF SPARKES AND GTRC "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL PAUL MATTES, JEFF SPARKES OR
 * GTRC BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* 
 * tclAppInit.c --
 *
 *	Provides a default version of the main program and Tcl_AppInit
 *	procedure for Tcl applications (without Tk).
 *
 * Copyright (c) 1993 The Regents of the University of California.
 * Copyright (c) 1994-1997 Sun Microsystems, Inc.
 * Copyright (c) 1998-1999 by Scriptics Corporation.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: tcl3270.c,v 1.35 2007/07/17 15:58:53 pdm Exp $
 */

/*
 *	tcl3270.c
 *		A Tcl-based 3270 Terminal Emulator
 *		Main proceudre.
 */

#include "tcl.h"

#include "globals.h"

#include <sys/types.h>
#include <sys/wait.h>

#include "s3270_proto.h"

#if TCL_MAJOR_VERSION > 8 || TCL_MINOR_VERSION >= 6 /*[*/
# define NEED_PTHREADS 1
# include <pthread.h>
#endif /*]*/

#define IBS	4096
/*
 * The following variable is a special hack that is needed in order for
 * Sun shared libraries to be used for Tcl.
 */

#if defined(_sun) /*[*/
extern int matherr();
int *tclDummyMathPtr = (int *) matherr;
#endif /*]*/

static int s3270pipe[2];
static bool verbose = false;
static bool interactive = false;
static pid_t s3270_pid;
static bool s3270_exited = false;
static char s3270_errmsg[1024];
#if defined(NEED_PTHREADS) /*[*/
static pthread_mutex_t cmd_mutex;
#endif /*]*/

static Tcl_ObjCmdProc x3270_cmd;
static Tcl_ObjCmdProc Rows_cmd, Cols_cmd, Status_cmd;
static int tcl3270_main(Tcl_Interp *interp, int argc, const char *argv[]);

/*
 *----------------------------------------------------------------------
 *
 * main --
 *
 *	This is the main program for the application.
 *
 * Results:
 *	None: Tcl_Main never returns here, so this procedure never
 *	returns either.
 *
 * Side effects:
 *	Whatever the application does.
 *
 *----------------------------------------------------------------------
 */
int
main(int argc, char **argv)
{
    Tcl_Main(argc, argv, Tcl_AppInit);
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_AppInit --
 *
 *	This procedure performs application-specific initialization.
 *	Most applications, especially those that incorporate additional
 *	packages, will have their own version of this procedure.
 *
 * Results:
 *	Returns a standard Tcl completion code, and leaves an error
 *	message in the interp's result if an error occurs.
 *
 * Side effects:
 *	Depends on the startup script.
 *
 *----------------------------------------------------------------------
 */
int
Tcl_AppInit(Tcl_Interp *interp)
{
    const char *s0, *s;
    int tcl_argc;
    const char **tcl_argv;
    int argc;
    const char **argv;
    unsigned i;
    int j;
    Tcl_Obj *argv_obj;
    static char nbuf[256];

    if (Tcl_Init(interp) == TCL_ERROR) {
	return TCL_ERROR;
    }

    /* Use argv and argv0 to figure out our command-line arguments. */
    s0 = Tcl_GetVar(interp, "argv0", 0);
    if (s0 == NULL) {
	return TCL_ERROR;
    }
    s = Tcl_GetVar(interp, "argv", 0);
    if (s == NULL) {
	return TCL_ERROR;
    }
    (void) Tcl_SplitList(interp, s, &tcl_argc, &tcl_argv);
    argc = tcl_argc + 1;
    argv = (const char **)Malloc((argc + 1) * sizeof(char *));
    argv[0] = s0;
    for (j = 0; j < tcl_argc; j++) {
	argv[1 + j] = tcl_argv[j];
    }
    argv[argc] = NULL;

    /* Find out if we're interactive. */
    s = Tcl_GetVar(interp, "tcl_interactive", 0);
    interactive = (s != NULL && !strcmp(s, "1"));

    /* Call main. */
    if (tcl3270_main(interp, argc, argv) == TCL_ERROR) {
	return TCL_ERROR;
    }

    /* Replace Tcl's argc and argv with whatever was left. */
    argv_obj = Tcl_NewListObj(0, NULL);
    for (i = 1; argv[i] != NULL; i++) {
	Tcl_ListObjAppendElement(interp, argv_obj, Tcl_NewStringObj(argv[i],
		strlen(argv[i])));
    }
    Tcl_SetVar2Ex(interp, "argv", NULL, argv_obj, 0);
    sprintf(nbuf, "%d", i? i - 1 : 0);
    Tcl_SetVar(interp, "argc", nbuf, 0);

    /*
     * Call the init procedures for included packages.  Each call should
     * look like this:
     *
     * if (Mod_Init(interp) == TCL_ERROR) {
     *     return TCL_ERROR;
     * }
     *
     * where "Mod" is the name of the module.
     */

    /*
     * Specify a user-specific startup file to invoke if the application
     * is run interactively.  Typically the startup file is "~/.apprc"
     * where "app" is the name of the application.  If this line is deleted
     * then no user-specific startup file will be run under any conditions.
     */
#if 0
    Tcl_SetVar(interp, "tcl_rcFileName", "~/.tclshrc", TCL_GLOBAL_ONLY);
#endif

    return TCL_OK;
}

void
usage(const char *msg)
{
    if (msg != NULL) {
	fprintf(stderr, "%s\n", msg);
    }
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  tcl3270 [single-option]\n");
    fprintf(stderr, "  tcl3270 [script [script-args]] [-- [tcl3270-options] [s3270-options] [host|session-file.tcl3270]]\n");
    fprintf(stderr, "single-options:\n");
    fprintf(stderr, "  --help      display usage\n");
    fprintf(stderr, "  -v          display version\n");
    fprintf(stderr, "  --version   display version\n");
    fprintf(stderr, "  -?          display usage\n");
    fprintf(stderr, "tcl3270-options:\n");
    fprintf(stderr, "  -d          debug s3270 I/O\n");
    exit(1);
}

/* Do a single command, and interpret the results. */
static int
run_s3270(const char *cmd, bool *success, char **status, char **ret)
{
    int st;
    int nw = 0;
    char buf[IBS];
    char rbuf[IBS];
    int sl = 0;
    size_t nr;
    bool complete = false;
    char *cmd_nl;
    size_t ret_sl = 0;
    char *nl;
    int rv = -1;

    *success = false;
    if (status != NULL) {
	*status = NULL;
    }
    *ret = NULL;

#if defined(NEED_PTHREADS) /*[*/
    pthread_mutex_lock(&cmd_mutex);
#endif /*]*/

    /* Check s3270. */
    if (s3270_exited) {
	*ret = NewString(s3270_errmsg);
	rv = 0;
	goto done;
    }
    if (waitpid(s3270_pid, &st, WNOHANG) > 0) {
	s3270_exited = true;
	if (WIFEXITED(st)) {
	    snprintf(s3270_errmsg, sizeof(s3270_errmsg),
		    "s3270 exited with status %d", WEXITSTATUS(st));
	} else if (WIFSIGNALED(st)) {
	    snprintf(s3270_errmsg, sizeof(s3270_errmsg),
		    "s3270 killed by signal %d", WTERMSIG(st));
	} else {
	    snprintf(s3270_errmsg, sizeof(s3270_errmsg),
		    "Unknown s3270 exit status %d", st);
	}
	*ret = NewString(s3270_errmsg);
	rv = 0;
	goto done;
    }

    /* Speak to s3270. */
    if (verbose) {
	fprintf(stderr, "i+ out %s\n", (cmd != NULL) ? cmd : "");
    }

    cmd_nl = Malloc(strlen(cmd) + 2);
    sprintf(cmd_nl, "%s\n", cmd);

    nw = write(s3270pipe[1], cmd_nl, strlen(cmd_nl));
    if (nw < 0) {
	perror("s3270 (back end): write");
	Free(cmd_nl);
	goto done;
    }
    Free(cmd_nl);

    /* Get the answer. */
    while (!complete && (nr = read(s3270pipe[0], rbuf, IBS)) > 0) {
	size_t i;
	bool get_more = false;

	i = 0;
	do {
	    /* Copy from rbuf into buf until '\n'. */
	    while (i < nr && rbuf[i] != '\n') {
		if (sl < IBS - 1) {
		    buf[sl++] = rbuf[i++];
		}
	    }
	    if (rbuf[i] == '\n') {
		i++;
	    } else {
		/* Go get more input. */
		get_more = true;
		break;
	    }

	    /* Process one line of output. */
	    buf[sl] = '\0';

	    if (verbose) {
		fprintf(stderr, "i+ in %s\n", buf);
	    }
	    if (!strcmp(buf, PROMPT_OK)) {
		*success = true;
		complete = true;
		break;
	    } else if (!strcmp(buf, PROMPT_ERROR)) {
		*success = false;
		complete = true;
		break;
	    } else if (!strncmp(buf, DATA_PREFIX, strlen(DATA_PREFIX))) {
		*ret = Realloc(*ret, ret_sl + strlen(buf +
			    strlen(DATA_PREFIX)) + 2);
		*(*ret + ret_sl) = '\0';
		strcat(strcat(*ret, buf + strlen(DATA_PREFIX)), "\n");
		ret_sl += strlen(buf + strlen(DATA_PREFIX)) + 1;
	    } else if (status != NULL) {
		*status = NewString(buf);
	    }

	    /* Get ready for the next. */
	    sl = 0;
	} while (i < nr);

	if (get_more) {
	    get_more = false;
	    continue;
	}
    }
    if (nr < 0) {
	perror("s3270 (back end) read");
	if (status != NULL && *status != NULL) {
	    Free(*status);
	    *status = NULL;
	}
	if (*ret != NULL) {
	    Free(*ret);
	    *ret = NULL;
	}
	goto done;
    } else if (nr == 0) {
	if (verbose) {
	    fprintf(stderr, "s3270 EOF\n");
	}
	exit(0);
    }

    /* Make sure we return someting. */
    if (*ret == NULL) {
	*ret = NewString("");
    }

    /* Remove any trailing newline. */
    if ((nl = strrchr(*ret, '\n')) != NULL && !*(nl + 1)) {
	*nl = '\0';
    }

    rv = 0;
done:
#if defined(NEED_PTHREADS) /*[*/
    pthread_mutex_unlock(&cmd_mutex);
#endif /*]*/
    return rv;
}

/* Initialization procedure for tcl3270. */
static int
tcl3270_main(Tcl_Interp *interp, int argc, const char *argv[])
{
    char **nargv = Calloc(argc + 7, sizeof(char *));
    int i_in, i_out = 0;
    int to_s3270_pipe[2];
    int from_s3270_pipe[2];
    bool success;
    char *ret;
    char *action;
    char *paren;
    int skip_ix = -1;

    /*
     * Handle special first arguments first, which completely violate the
     * convention below, but give people a chance to figure out how the command
     * works without having a manpage.
     */
    if (argc > 1) {
	if (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version")) {
	    fprintf(stderr, "%s\n", build);
	    exit(0);
	}
	if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-?")) {
	    usage(NULL);
	}
    }

    /*
     * The syntax, dictated by tclsh, is:
     *   [script script-args] [-- [tcl3270-args] [host[:port]]]
     * I.e., the optional script name and arguments come first, then an
     * optional '--', then tcl3270's arguments.
     *
     * Find the '--'.
     */
    for (i_in = 1; i_in < argc; i_in++) {
	if (!strcmp(argv[i_in], "--")) {
	    skip_ix = i_in;
	    argv[skip_ix] = NULL;
	    break;
	}
    }

    /*
     * Pick off '-d', which is the only tcl3270-specific option besides -v/-?.
     */
    if (skip_ix >= 0 && argc > skip_ix + 1
	    && !strcmp(argv[skip_ix + 1], "-d")) {
	skip_ix++;
	verbose = true;
    }

    /* Set up s3270's command-line arguments. */
    nargv[i_out++] = "s3270";
    nargv[i_out++] = "-utf8";
    nargv[i_out++] = "-minversion";
    nargv[i_out++] = "4.0";
    nargv[i_out++] = "-alias";
    nargv[i_out++] = "tcl3270";
    if (skip_ix >= 0) {
	for (i_in = skip_ix + 1; i_in < argc; i_in++) {
	    nargv[i_out++] = (char *)argv[i_in];
	}
    }
    nargv[i_out++] = NULL;

    /* Set up pipes. */
    if (pipe(to_s3270_pipe) < 0 || pipe(from_s3270_pipe) < 0) {
	perror("pipe");
	return TCL_ERROR;
    }

    /* Start s3270. */
    switch (s3270_pid = fork()) {
    case -1:
	perror("fork");
	return TCL_ERROR;
    case 0:
	/* Child. */

	/* Redirect I/O. */
	close(to_s3270_pipe[1]);
	if (dup2(to_s3270_pipe[0], 0) < 0) {
	    perror("dup2");
	    exit(1);
	}
	close(to_s3270_pipe[0]);
	if (dup2(from_s3270_pipe[1], 1) < 0) {
	    perror("dup2");
	    exit(1);
	}
	close(from_s3270_pipe[1]);

	/* Run s3270. */
	if (execvp("s3270", nargv) < 0) {
	    perror("s3270 (back end)");
	    exit(1);
	}
	break;
    default:
	/* Parent. */
	break;
    }

    /* Redirect I/O. */
    close(to_s3270_pipe[0]);
    close(from_s3270_pipe[1]);
    s3270pipe[0] = from_s3270_pipe[0];
    s3270pipe[1] = to_s3270_pipe[1];

#if defined(NEED_PTHREADS) /*[*/
    /* Set up the mutex. */
    pthread_mutex_init(&cmd_mutex, NULL);
#endif /*]*/

    /* Run 'Actions()' to learn what Tcl commands we need to add. */
    if (run_s3270("Actions()", &success, NULL, &ret) < 0) {
	return TCL_ERROR;
    }
    if (!success) {
	fprintf(stderr, "Actions() failed:\n%s\n", ret);
	return TCL_ERROR;
    }

    /* Create the actions. */
    action = ret;
    while ((paren = strchr(action, '(')) != NULL) {

	/* Create the command. */
	*paren = '\0';
	if (Tcl_CreateObjCommand(interp, action, x3270_cmd, NULL, NULL)
		== NULL) {
	    return TCL_ERROR;
	}

	/* Skip to the next action. */
	paren++;
	if (*paren == ')') {
	    paren++;
	}
	if (*paren == ' ') {
	    paren++;
	}
	action = paren;
    }

    Free(ret);

    /* Create some locally-defined actions. */
    if (Tcl_CreateObjCommand(interp, "Rows", Rows_cmd, NULL, NULL) == NULL) {
	return TCL_ERROR;
    }
    if (Tcl_CreateObjCommand(interp, "Cols", Cols_cmd, NULL, NULL) == NULL) {
	return TCL_ERROR;
    }
    if (Tcl_CreateObjCommand(interp, "Status", Status_cmd, NULL, NULL)
	    == NULL) {
	return TCL_ERROR;
    }

    return TCL_OK;
}

/* Quote a string according to Xt event map argument syntax. */
static char *
quoted(const char *arg)
{
    static char quoted_chars[] = " ,()";
    int i;
    bool needed = false;
    char *ret;
    char *out;
    char c;
    char last = '\0';

    if (!*arg) {
	/* Empty string -> quoted. */
	return NewString("\"\"");
    }

    /*
     * Check if it contains a character that triggers requires quoting,
     * or starts with a '"'.
     */
    for (i = 0; quoted_chars[i]; i++) {
	if (strchr(arg, quoted_chars[i]) != NULL) {
	    needed = true;
	    break;
	}
    }
    if (!needed && arg[0] != '"') {
	return NewString(arg);
    }

    /*
     * Replace double quotes with a backslash and a double quote.
     * Replace a backslash at the end with a double backslash.
     * Wrap the whole thing in double quotes.
     */

    /*
     * Allocate enough memory for:
     *  opening double quote
     *  every character needing a backslash in front of it
     *  trailing backslash needing to be doubled
     *  trailing double quote
     *  terminating NUL
     */
    ret = out = Malloc(1 + (strlen(arg) * 2) + 1 + 1 + 1);
    *out++ = '"';
    while ((c = *arg++)) {
	last = c;
	if (c == '"') {
	    *out++ = '\\';
	}
	*out++ = c;
    }
    if (last == '\\') {
	*out++ = '\\';
    }
    *out++ = '"';
    *out = '\0';
    return ret;
}

/* The Tcl "x3270" command: The root of all 3270 access. */
static int
x3270_cmd(ClientData clientData, Tcl_Interp *interp, int objc,
	Tcl_Obj *CONST objv[])
{
    int i;
    size_t len = 0;
    char *cmd;
    bool success;
    int rv;
    char *ret;
    char *rest;
    char *nl;
    Tcl_Obj *o = NULL;

    /* Marshal the arguments. */
    for (i = 0; i < objc; i++) {
	len += 1 + 2 * strlen(Tcl_GetString(objv[i]));
    }
    len += 3; /* parens and trailing NUL */
    cmd = Malloc(len);
    strcpy(cmd, Tcl_GetString(objv[0]));
    strcat(cmd, "(");
    for (i = 1; i < objc; i++) {
	char *q;

	if (i > 1) {
	    strcat(cmd, ",");
	}
	q = quoted(Tcl_GetString(objv[i]));
	strcat(cmd, q);
	Free(q);
    }
    strcat(cmd, ")");

    /* Run the action. */
    rv = run_s3270(cmd, &success, NULL, &ret);
    if (rv < 0) {
	Free(cmd);
	Tcl_SetResult(interp, "Internal error", TCL_STATIC);
	return TCL_ERROR;
    }

    Free(cmd);
    if (!success) {
	Tcl_SetResult(interp, ret, TCL_VOLATILE);
	Free(ret);
	return TCL_ERROR;
    }

    /* If the output is on one line, return it as a string. */
    if (strchr(ret, '\n') == NULL) {
	Tcl_SetResult(interp, ret, TCL_VOLATILE);
	Free(ret);
	return TCL_OK;
    }

    /* Return it as a list. */
    o = Tcl_NewListObj(0, NULL);
    rest = ret;
    while ((nl = strchr(rest, '\n')) != NULL) {
	*nl = '\0';
	Tcl_ListObjAppendElement(interp, o, Tcl_NewStringObj(rest, -1));
	rest = nl + 1;
    }
    Tcl_ListObjAppendElement(interp, o, Tcl_NewStringObj(rest, -1));
    Tcl_SetObjResult(interp, o);
    Free(ret);
    return TCL_OK;
}

/* Return the status line. */
static int
Status_cmd(ClientData clientData, Tcl_Interp *interp, int objc,
	Tcl_Obj *CONST objv[])
{
    bool success;
    char *status;
    char *ret;

    if (run_s3270("", &success, &status, &ret) < 0) {
	Tcl_SetResult(interp, "Internal error", TCL_STATIC);
	return TCL_ERROR;
    }
    Tcl_SetResult(interp, status, TCL_VOLATILE);
    Free(status);
    Free(ret);
    return TCL_OK;
}

/**
 * Isolate a field within the status line.
 *
 * @param[in] status	Status line.
 * @param[in] index	1-origin field index.
 */
static char *
field(const char *status, int index)
{
    char *space;
    const char *s = status;
    size_t len;
    char *ret;

    while ((space = strchr(s, ' ')) && --index > 0) {
	s = space + 1;
    }
    if (!*s) {
	return NewString("");
    }
    if ((space = strchr(s, ' ')) == NULL) {
	return NewString(s);
    }
    len = space - s;
    ret = Malloc(len + 1);
    strncpy(ret, s, len);
    ret[len] = '\0';
    return ret;
}

/* Report the number of rows. */
static int
Rows_cmd(ClientData clientData, Tcl_Interp *interp, int objc,
	Tcl_Obj *CONST objv[])
{
    bool success;
    char *status;
    char *ret;
    char *f;

    if (run_s3270("", &success, &status, &ret) < 0) {
	Tcl_SetResult(interp, "Internal error", TCL_STATIC);
	return TCL_ERROR;
    }
    Free(ret);
    f = field(status, 7);
    Free(status);
    Tcl_SetResult(interp, f, TCL_VOLATILE);
    Free(f);
    return TCL_OK;
}

/* Report the number of columns. */
static int
Cols_cmd(ClientData clientData, Tcl_Interp *interp, int objc,
	Tcl_Obj *CONST objv[])
{
    bool success;
    char *status;
    char *ret;
    char *f;

    if (run_s3270("", &success, &status, &ret) < 0) {
	Tcl_SetResult(interp, "Internal error", TCL_STATIC);
	return TCL_ERROR;
    }
    Free(ret);
    f = field(status, 8);
    Free(status);
    Tcl_SetResult(interp, f, TCL_VOLATILE);
    Free(f);
    return TCL_OK;
}

/* Error abort used for Malloc failures. */
void
Error(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(1);
}
