/* vi: ts=8 sts=4 sw=4
 *
 * $Id$
 *
 * This file is part of the KDE project, module kdesu.
 * Copyright (C) 1999 Geert Jansen <g.t.jansen@stud.tue.nl>
 *
 * handler.cpp: A connection handler for kdesud.
 */


#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <qcstring.h>

#include <kdebug.h>
#include "handler.h"
#include "repo.h"
#include "lexer.h"
#include "secure.h"
#include "process.h"


// Global repository
extern Repository *repo;


ConnectionHandler::ConnectionHandler(int fd)
	: SocketSecurity(fd), mFd(fd)
{
    mbPass = false;
    mbUser = false;
}

ConnectionHandler::~ConnectionHandler()
{
    mBuf.fill('x');
    if (mbPass)
	mPass.fill('x');
}

void ConnectionHandler::respond(int ok, QCString s)
{
    QCString buf;

    switch (ok) {
    case Res_OK:
	buf = "OK";
	break;
    case Res_NO:
	buf = "NO";
	break;
    default:
	assert(1);
    }

    if (!s.isEmpty()) {
	buf += ' ';
	buf += s;
    }
    buf += '\n';

    send(mFd, buf.data(), buf.length(), 0);
}


/**
 * Parse and do one command. On a parse error, return -1. This will 
 * close the socket in the main accept loop.
 */

int ConnectionHandler::doCommand()
{
    if ((uid_t) peerUid() != getuid()) {
	qWarning("Peer uid not equal to me");
	qWarning("Peer: %d, Me: %d", peerUid(), (int) getuid());
	return -1;
    }

    int n = mBuf.find('\n');
    assert(n != -1);

    QCString newbuf = mBuf.mid(0, n+1);
    for (int i=0; i<=n; i++)
	mBuf[n] = 'x';
    mBuf.remove(0, n+1);

    /* I'd like to use auto_ptr here, but egcs 1.1.2 has it
     * commented out.. */

    Lexer *l = new Lexer(newbuf);

    int tok = l->lex();

    QCString key, command, pass;
    Data_entry data;
    const Data_entry *pdata;

    switch (tok) {
    case Lexer::Tok_pass: 
	tok = l->lex();
	if (tok != Lexer::Tok_str)
	    goto parse_error;
	mPass = l->lval();
	mbPass = true;
	tok = l->lex();
	if (tok != Lexer::Tok_num)
	    goto parse_error;
	mTimeout = l->lval().toInt();
	if (l->lex() != '\n')
	    goto parse_error;
	respond(Res_OK);
	kDebugInfo("Password set!");
	break;

    case Lexer::Tok_user:
	tok = l->lex();
	if (tok != Lexer::Tok_str)
	    goto parse_error;
	mUser = l->lval();
	mbUser = true;
	if (l->lex() != '\n')
	    goto parse_error;
	respond(Res_OK);
	kDebugInfo("User set to %s", (const char *) mUser);
	break;

    case Lexer::Tok_exec:
    {
	tok = l->lex();
	if (tok != Lexer::Tok_str)
	    goto parse_error;
	command = l->lval();
	if (l->lex() != '\n')
	    goto parse_error;

	if (!mbUser)
	    goto parse_error;
	key = mUser;
	key += "*";
	key += command;
	
	pdata = repo->find(key);
	if (!pdata) {
	    if (!mbPass) {
		respond(Res_NO);
		break;
	    }
	    data.value = mPass;
	    data.timeout = mTimeout;
	    repo->add(key, data);
	    pass = mPass;
	} else
	    pass = pdata->value;

	// Check if we need to rebuild the sycoca.
	bool build_sycoca = true;
	key = QCString("ksycoca_") + mUser;
	pdata = repo->find(key);
	if (pdata && (pdata->value == "yes")) {
	    kDebugInfo("kdesud: NOT building sycoca");
	    build_sycoca = false;
	} else {
	    kDebugInfo("kdesud: building sycoca");
	    data.value = QCString("yes");
	    data.timeout = 86400;
	    repo->add(key, data);
	}

	// Execute the command asynchronously
	kDebugInfo("Executing command: %s", (const char *) command);
	pid_t pid = fork();
	if (pid < 0) {
	    qWarning("fork(): %s", strerror(errno));
	    respond(Res_NO);
	    break;
	} else if (pid > 0) {
	    respond(Res_OK);
	    break;
	}

	// Ignore SIGCHLD because "class SuProcess" needs waitpid()
	signal(SIGCHLD, SIG_DFL);

	SuProcess proc;
	proc.setBuildSycoca(build_sycoca);
	proc.setCommand(command);
	proc.setUser(mUser);
	int ret = proc.exec(pass.data());
	kDebugInfo("Command completed");
	_exit(ret);
    }

    case Lexer::Tok_del:
	tok = l->lex();
	if (tok != Lexer::Tok_str)
	    goto parse_error;
	command = l->lval();
	if (l->lex() != '\n')
	    goto parse_error;
	
	if (!mbUser)
	    goto parse_error;
	key = mUser;
	key += "*";
	key += command;

	pdata = repo->find(key);
	if (!pdata) {
	    respond(Res_NO);
	    break;
	}
	repo->remove(key);
	respond(Res_OK);
	kDebugInfo("Deleted key: %s", (const char *) key);
	break;

    case Lexer::Tok_set:
	tok = l->lex();
	if (tok != Lexer::Tok_str)
	    goto parse_error;
	key = l->lval();
	tok = l->lex();
	if (tok != Lexer::Tok_str)
	    goto parse_error;
	data.value = l->lval();
	if (l->lex() != '\n')
	    goto parse_error;
	data.timeout = 86400;
	repo->add(key, data);
	kDebugInfo("stored key: %s", key.data());
	respond(Res_OK);
	break;

    case Lexer::Tok_get:
	tok = l->lex();
	if (tok != Lexer::Tok_str)
	    goto parse_error;
	key = l->lval();
	if (l->lex() != '\n')
	    goto parse_error;
	kDebugInfo("request for key: %s", key.data());
	pdata = repo->find(key);
	if (pdata)
	    respond(Res_OK, pdata->value);
	else
	    respond(Res_NO);
	break;
	
    case Lexer::Tok_ping:
	tok = l->lex();
	if (tok != '\n')
	    goto parse_error;
	respond(Res_OK);
	kDebugInfo("PING");
	break;

    case Lexer::Tok_stop:
	tok = l->lex();
	if (tok != '\n')
	    goto parse_error;
	respond(Res_OK);
	kDebugInfo("Stopping by command");
	exit(0);

    default:
	kDebugInfo("Uknown command: %s", (const char *) l->lval());
	goto parse_error;
    }

    delete l;
    return 0;

parse_error:
    kDebugInfo("Parse error");
    delete l;
    return -1;
}


/**
 * Handle a connection: make sure we don't block
 */

int ConnectionHandler::handle()
{
    int ret, nbytes;

    /* Add max # bytes to connection buffer */

    char tmpbuf[100];
    nbytes = recv(mFd, tmpbuf, 99, 0);

    if (nbytes < 0) {
	if (errno == EINTR)
	    return 0;
	// read error
	return -1;
    } else if (nbytes == 0) {
	// eof
	kDebugInfo("eof on fd %d", mFd);
	return -1;
    }
    tmpbuf[nbytes] = '\000';

    if (mBuf.length()+nbytes > 500) {
	qWarning("line too long");
	return -1;
    }

    mBuf += tmpbuf;
    memset(tmpbuf, 'x', nbytes);

    
    /* Do we have a complete command yet? */

    while (mBuf.find('\n') != -1) {
	ret = doCommand();
	if (ret < 0)
	    return ret;
    }

    return 0;
}

