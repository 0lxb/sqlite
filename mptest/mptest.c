/*
** 2013-04-05
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** 
** This is a program used for testing SQLite, and specifically for testing
** the ability of independent processes to access the same SQLite database
** concurrently.
**
** Compile this program as follows:
**
**    gcc -g -c -Wall sqlite3.c $(OPTS)
**    gcc -g -o mptest mptest.c sqlite3.o $(LIBS)
**
** Recommended options:
**
**    -DHAVE_USLEEP
**    -DSQLITE_NO_SYNC
**    -DSQLITE_THREADSAFE=0
**    -DSQLITE_OMIT_LOAD_EXTENSION
**
** Run like this:
**
**     ./mptest $database $script
**
** where $database is the database to use for testing and $script is a
** test script.
*/
#include "sqlite3.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>

/* Global data
*/
static struct Global {
  char *argv0;           /* Name of the executable */
  const char *zVfs;      /* Name of VFS to use. Often NULL meaning "default" */
  char *zDbFile;         /* Name of the database */
  sqlite3 *db;           /* Open connection to database */
  char *zErrLog;         /* Filename for error log */
  FILE *pErrLog;         /* Where to write errors */
  char *zLog;            /* Name of output log file */
  FILE *pLog;            /* Where to write log messages */
  char zName[12];        /* Symbolic name of this process */
  int taskId;            /* Task ID.  0 means supervisor. */
  int iTrace;            /* Tracing level */
  int bSqlTrace;         /* True to trace SQL commands */
  int nError;            /* Number of errors */
  int nTest;             /* Number of --match operators */
  int iTimeout;          /* Milliseconds until a busy timeout */
} g;

/* Default timeout */
#define DEFAULT_TIMEOUT 10000

/*
** Print a message adding zPrefix[] to the beginning of every line.
*/
static void printWithPrefix(FILE *pOut, const char *zPrefix, const char *zMsg){
  while( zMsg && zMsg[0] ){
    int i;
    for(i=0; zMsg[i] && zMsg[i]!='\n' && zMsg[i]!='\r'; i++){}
    fprintf(pOut, "%s%.*s\n", zPrefix, i, zMsg);
    zMsg += i;
    while( zMsg[0]=='\n' || zMsg[0]=='\r' ) zMsg++;
  }
}

/*
** Compare two pointers to strings, where the pointers might be NULL.
*/
static int safe_strcmp(const char *a, const char *b){
  if( a==b ) return 0;
  if( a==0 ) return -1;
  if( b==0 ) return 1;
  return strcmp(a,b);
}

/*
** Return TRUE if string z[] matches glob pattern zGlob[].
** Return FALSE if the pattern does not match.
**
** Globbing rules:
**
**      '*'       Matches any sequence of zero or more characters.
**
**      '?'       Matches exactly one character.
**
**     [...]      Matches one character from the enclosed list of
**                characters.
**
**     [^...]     Matches one character not in the enclosed list.
**
**      '#'       Matches any sequence of one or more digits with an
**                optional + or - sign in front
*/
int strglob(const char *zGlob, const char *z){
  int c, c2;
  int invert;
  int seen;

  while( (c = (*(zGlob++)))!=0 ){
    if( c=='*' ){
      while( (c=(*(zGlob++))) == '*' || c=='?' ){
        if( c=='?' && (*(z++))==0 ) return 0;
      }
      if( c==0 ){
        return 1;
      }else if( c=='[' ){
        while( *z && strglob(zGlob-1,z) ){
          z++;
        }
        return (*z)!=0;
      }
      while( (c2 = (*(z++)))!=0 ){
        while( c2!=c ){
          c2 = *(z++);
          if( c2==0 ) return 0;
        }
        if( strglob(zGlob,z) ) return 1;
      }
      return 0;
    }else if( c=='?' ){
      if( (*(z++))==0 ) return 0;
    }else if( c=='[' ){
      int prior_c = 0;
      seen = 0;
      invert = 0;
      c = *(z++);
      if( c==0 ) return 0;
      c2 = *(zGlob++);
      if( c2=='^' ){
        invert = 1;
        c2 = *(zGlob++);
      }
      if( c2==']' ){
        if( c==']' ) seen = 1;
        c2 = *(zGlob++);
      }
      while( c2 && c2!=']' ){
        if( c2=='-' && zGlob[0]!=']' && zGlob[0]!=0 && prior_c>0 ){
          c2 = *(zGlob++);
          if( c>=prior_c && c<=c2 ) seen = 1;
          prior_c = 0;
        }else{
          if( c==c2 ){
            seen = 1;
          }
          prior_c = c2;
        }
        c2 = *(zGlob++);
      }
      if( c2==0 || (seen ^ invert)==0 ) return 0;
    }else if( c=='#' ){
      if( (z[0]=='-' || z[0]=='+') && isdigit(z[1]) ) z++;
      if( !isdigit(z[0]) ) return 0;
      z++;
      while( isdigit(z[0]) ){ z++; }
    }else{
      if( c!=(*(z++)) ) return 0;
    }
  }
  return *z==0;
}

/*
** Close output stream pOut if it is not stdout or stderr
*/
static void maybeClose(FILE *pOut){
  if( pOut!=stdout && pOut!=stderr ) fclose(pOut);
}

/*
** Print an error message
*/
static void errorMessage(const char *zFormat, ...){
  va_list ap;
  char *zMsg;
  char zPrefix[30];
  va_start(ap, zFormat);
  zMsg = sqlite3_vmprintf(zFormat, ap);
  va_end(ap);
  sqlite3_snprintf(sizeof(zPrefix), zPrefix, "%s:ERROR: ", g.zName);
  if( g.pLog ){
    printWithPrefix(g.pLog, zPrefix, zMsg);
    fflush(g.pLog);
  }
  if( g.pErrLog && safe_strcmp(g.zErrLog,g.zLog) ){
    printWithPrefix(g.pErrLog, zPrefix, zMsg);
    fflush(g.pErrLog);
  }
  sqlite3_free(zMsg);
  g.nError++;
}

/* Forward declaration */
static int trySql(const char*, ...);

/*
** Print an error message and then quit.
*/
static void fatalError(const char *zFormat, ...){
  va_list ap;
  char *zMsg;
  char zPrefix[30];
  va_start(ap, zFormat);
  zMsg = sqlite3_vmprintf(zFormat, ap);
  va_end(ap);
  sqlite3_snprintf(sizeof(zPrefix), zPrefix, "%s:FATAL: ", g.zName);
  if( g.pLog ){
    printWithPrefix(g.pLog, zPrefix, zMsg);
    fflush(g.pLog);
    maybeClose(g.pLog);
  }
  if( g.pErrLog && safe_strcmp(g.zErrLog,g.zLog) ){
    printWithPrefix(g.pErrLog, zPrefix, zMsg);
    fflush(g.pErrLog);
    maybeClose(g.pErrLog);
  }
  sqlite3_free(zMsg);
  if( g.db ){
    int nTry = 0;
    g.iTimeout = 0;
    while( trySql("UPDATE client SET wantHalt=1;")==SQLITE_BUSY
           && (nTry++)<100 ){
      sqlite3_sleep(10);
    }
  }
  sqlite3_close(g.db);
  exit(1);  
}


/*
** Print a log message
*/
static void logMessage(const char *zFormat, ...){
  va_list ap;
  char *zMsg;
  char zPrefix[30];
  va_start(ap, zFormat);
  zMsg = sqlite3_vmprintf(zFormat, ap);
  va_end(ap);
  sqlite3_snprintf(sizeof(zPrefix), zPrefix, "%s: ", g.zName);
  if( g.pLog ){
    printWithPrefix(g.pLog, zPrefix, zMsg);
    fflush(g.pLog);
  }
  sqlite3_free(zMsg);
}

/*
** Return the length of a string omitting trailing whitespace
*/
static int clipLength(const char *z){
  int n = (int)strlen(z);
  while( n>0 && isspace(z[n-1]) ){ n--; }
  return n;
}

/*
** Busy handler with a g.iTimeout-millisecond timeout
*/
static int busyHandler(void *pCD, int count){
  if( count*10>g.iTimeout ){
    if( g.iTimeout>0 ) errorMessage("timeout after %dms", g.iTimeout);
    return 0;
  }
  sqlite3_sleep(10);
  return 1;
}

/*
** SQL Trace callback
*/
static void sqlTraceCallback(void *NotUsed1, const char *zSql){
  logMessage("[%.*s]", clipLength(zSql), zSql);
}

/*
** SQL error log callback
*/
static void sqlErrorCallback(void *pArg, int iErrCode, const char *zMsg){
  if( (iErrCode&0xff)==SQLITE_SCHEMA && g.iTrace<3 ) return;
  errorMessage("(errcode=%d) %s", iErrCode, zMsg);
}

/*
** Prepare an SQL statement.  Issue a fatal error if unable.
*/
static sqlite3_stmt *prepareSql(const char *zFormat, ...){
  va_list ap;
  char *zSql;
  int rc;
  sqlite3_stmt *pStmt = 0;
  va_start(ap, zFormat);
  zSql = sqlite3_vmprintf(zFormat, ap);
  va_end(ap);
  rc = sqlite3_prepare_v2(g.db, zSql, -1, &pStmt, 0);
  if( rc!=SQLITE_OK ){
    sqlite3_finalize(pStmt);
    fatalError("%s\n%s\n", sqlite3_errmsg(g.db), zSql);
  }
  sqlite3_free(zSql);
  return pStmt;
}

/*
** Run arbitrary SQL.  Issue a fatal error on failure.
*/
static void runSql(const char *zFormat, ...){
  va_list ap;
  char *zSql;
  int rc;
  va_start(ap, zFormat);
  zSql = sqlite3_vmprintf(zFormat, ap);
  va_end(ap);
  rc = sqlite3_exec(g.db, zSql, 0, 0, 0);
  if( rc!=SQLITE_OK ){
    fatalError("%s\n%s\n", sqlite3_errmsg(g.db), zSql);
  }
  sqlite3_free(zSql);
}

/*
** Try to run arbitrary SQL.  Return success code.
*/
static int trySql(const char *zFormat, ...){
  va_list ap;
  char *zSql;
  int rc;
  va_start(ap, zFormat);
  zSql = sqlite3_vmprintf(zFormat, ap);
  va_end(ap);
  rc = sqlite3_exec(g.db, zSql, 0, 0, 0);
  sqlite3_free(zSql);
  return rc;
}

/* Structure for holding an arbitrary length string
*/
typedef struct String String;
struct String {
  char *z;         /* the string */
  int n;           /* Slots of z[] used */
  int nAlloc;      /* Slots of z[] allocated */
};

/* Free a string */
static void stringFree(String *p){
  if( p->z ) sqlite3_free(p->z);
  memset(p, 0, sizeof(*p));
}

/* Append n bytes of text to a string.  If n<0 append the entire string. */
static void stringAppend(String *p, const char *z, int n){
  if( n<0 ) n = (int)strlen(z);
  if( p->n+n>=p->nAlloc ){
    int nAlloc = p->nAlloc*2 + n + 100;
    char *z = sqlite3_realloc(p->z, nAlloc);
    if( z==0 ) fatalError("out of memory");
    p->z = z;
    p->nAlloc = nAlloc;
  }
  memcpy(p->z+p->n, z, n);
  p->n += n;
  p->z[p->n] = 0;
}

/* Reset a string to an empty string */
static void stringReset(String *p){
  if( p->z==0 ) stringAppend(p, " ", 1);
  p->n = 0;
  p->z[0] = 0;
}

/* Append a new token onto the end of the string */
static void stringAppendTerm(String *p, const char *z){
  int i;
  if( p->n ) stringAppend(p, " ", 1);
  if( z==0 ){
    stringAppend(p, "nil", 3);
    return;
  }
  for(i=0; z[i] && !isspace(z[i]); i++){}
  if( i>0 && z[i]==0 ){
    stringAppend(p, z, i);
    return;
  }
  stringAppend(p, "'", 1);
  while( z[0] ){
    for(i=0; z[i] && z[i]!='\''; i++){}
    if( z[i] ){
      stringAppend(p, z, i+1);
      stringAppend(p, "'", 1);
      z += i+1;
    }else{
      stringAppend(p, z, i);
      break;
    }
  }
  stringAppend(p, "'", 1);
}

/*
** Callback function for evalSql()
*/
static int evalCallback(void *pCData, int argc, char **argv, char **azCol){
  String *p = (String*)pCData;
  int i;
  for(i=0; i<argc; i++) stringAppendTerm(p, argv[i]);
  return 0;
}

/*
** Run arbitrary SQL and record the results in an output string
** given by the first parameter.
*/
static int evalSql(String *p, const char *zFormat, ...){
  va_list ap;
  char *zSql;
  int rc;
  char *zErrMsg = 0;
  va_start(ap, zFormat);
  zSql = sqlite3_vmprintf(zFormat, ap);
  va_end(ap);
  assert( g.iTimeout>0 );
  rc = sqlite3_exec(g.db, zSql, evalCallback, p, &zErrMsg);
  sqlite3_free(zSql);
  if( rc ){
    char zErr[30];
    sqlite3_snprintf(sizeof(zErr), zErr, "error(%d)", rc);
    stringAppendTerm(p, zErr);
    if( zErrMsg ){
      stringAppendTerm(p, zErrMsg);
      sqlite3_free(zErrMsg);
    }
  }
  return rc;
}

/*
** Look up the next task for client iClient in the database.
** Return the task script and the task number and mark that
** task as being under way.
*/
static int startScript(
  int iClient,              /* The client number */
  char **pzScript,          /* Write task script here */
  int *pTaskId              /* Write task number here */
){
  sqlite3_stmt *pStmt = 0;
  int taskId;
  int rc;
  int totalTime = 0;

  *pzScript = 0;
  g.iTimeout = 0;
  while(1){
    rc = trySql("BEGIN IMMEDIATE");
    if( rc==SQLITE_BUSY ){
      sqlite3_sleep(10);
      totalTime += 10;
      continue;
    }
    if( rc!=SQLITE_OK ){
      fatalError("%s\nBEGIN IMMEDIATE", sqlite3_errmsg(g.db));
    }
    if( g.nError || g.nTest ){
      runSql("UPDATE counters SET nError=nError+%d, nTest=nTest+%d",
             g.nError, g.nTest);
      g.nError = 0;
      g.nTest = 0;
    }
    pStmt = prepareSql("SELECT 1 FROM client WHERE id=%d AND wantHalt",iClient);
    rc = sqlite3_step(pStmt);
    sqlite3_finalize(pStmt);
    if( rc==SQLITE_ROW ){
      runSql("DELETE FROM client WHERE id=%d", iClient);
      runSql("COMMIT");
      g.iTimeout = DEFAULT_TIMEOUT;
      return SQLITE_DONE;
    }
    pStmt = prepareSql(
              "SELECT script, id FROM task"
              " WHERE client=%d AND starttime IS NULL"
              " ORDER BY id LIMIT 1", iClient);
    rc = sqlite3_step(pStmt);
    if( rc==SQLITE_ROW ){
      int n = sqlite3_column_bytes(pStmt, 0);
      *pzScript = sqlite3_malloc( n+ 1 );
      strcpy(*pzScript, (const char*)sqlite3_column_text(pStmt, 0));
      *pTaskId = taskId = sqlite3_column_int(pStmt, 1);
      sqlite3_finalize(pStmt);
      runSql("UPDATE task"
             "   SET starttime=strftime('%%Y-%%m-%%d %%H:%%M:%%f','now')"
             " WHERE id=%d;", taskId);
      runSql("COMMIT;");
      g.iTimeout = DEFAULT_TIMEOUT;
      return SQLITE_OK;
    }
    sqlite3_finalize(pStmt);
    if( rc==SQLITE_DONE ){
      if( totalTime>30000 ){
        errorMessage("Waited over 30 seconds with no work.  Giving up.");
        runSql("DELETE FROM client WHERE id=%d; COMMIT;", iClient);
        sqlite3_close(g.db);
        exit(1);
      }
      runSql("COMMIT;");
      sqlite3_sleep(100);
      totalTime += 100;
      continue;
    }
    fatalError("%s", sqlite3_errmsg(g.db));
  }
  g.iTimeout = DEFAULT_TIMEOUT;
}

/*
** Mark a script as having finished.   Remove the CLIENT table entry
** if bShutdown is true.
*/
static int finishScript(int iClient, int taskId, int bShutdown){
  runSql("UPDATE task"
         "   SET endtime=strftime('%%Y-%%m-%%d %%H:%%M:%%f','now')"
         " WHERE id=%d;", taskId);
  if( bShutdown ){
    runSql("DELETE FROM client WHERE id=%d", iClient);
  }
  return SQLITE_OK;
}

/*
** Start up a client process for iClient, if it is not already
** running.  If the client is already running, then this routine
** is a no-op.
*/
static void startClient(int iClient){
  runSql("INSERT OR IGNORE INTO client VALUES(%d,0)", iClient);
  if( sqlite3_changes(g.db) ){
#if !defined(_WIN32)
    char *zSys;
    zSys = sqlite3_mprintf(
                 "%s \"%s\" --client %d --trace %d %s&",
                 g.argv0, g.zDbFile, iClient, g.iTrace,
                 g.bSqlTrace ? "--sqltrace " : "");
    system(zSys);
    sqlite3_free(zSys);
#endif
#if defined(_WIN32)
    char *argv[10];
    char zClient[20];
    char zTrace[20];
    argv[0] = g.argv0;
    argv[1] = g.zDbFile;
    argv[2] = "--client";
    sqlite3_snprintf(sizeof(zClient),zClient,"%d",iClient);
    argv[3] = zClient;
    argv[4] = "--trace";
    sqlite3_snprintf(sizeof(zTrace),zTrace,"%d",g.iTrace);
    argv[5] = zTrace;
    if( g.bSqlTrace ){
      argv[6] = "--sqltrace";
      argv[7] = 0;
    }else{
      argv[6] = 0;
    }
    _spawnv(_P_NOWAIT, g.argv0, argv);
#endif
  }
}

/*
** Read the entire content of a file into memory
*/
static char *readFile(const char *zFilename){
  FILE *in = fopen(zFilename, "rb");
  long sz;
  char *z;
  if( in==0 ){
    fatalError("cannot open \"%s\" for reading", zFilename);
  }
  fseek(in, 0, SEEK_END);
  sz = ftell(in);
  rewind(in);
  z = sqlite3_malloc( sz+1 );
  sz = (long)fread(z, 1, sz, in);
  z[sz] = 0;
  fclose(in);
  return z;
}

/*
** Return the length of the next token.
*/
static int tokenLength(const char *z, int *pnLine){
  int n = 0;
  if( isspace(z[0]) || (z[0]=='/' && z[1]=='*') ){
    int inC = 0;
    int c;
    if( z[0]=='/' ){
      inC = 1;
      n = 2;
    }
    while( (c = z[n++])!=0 ){
      if( c=='\n' ) (*pnLine)++;
      if( isspace(c) ) continue;
      if( inC && c=='*' && z[n]=='/' ){
        n++;
        inC = 0;
      }else if( !inC && c=='/' && z[n]=='*' ){
        n++;
        inC = 1;
      }else if( !inC ){
        break;
      }
    }
    n--;
  }else if( z[0]=='-' && z[1]=='-' ){
    for(n=2; z[n] && z[n]!='\n'; n++){}
    if( z[n] ){ (*pnLine)++; n++; }
  }else if( z[0]=='"' || z[0]=='\'' ){
    int delim = z[0];
    for(n=1; z[n]; n++){
      if( z[n]=='\n' ) (*pnLine)++;
      if( z[n]==delim ){
        n++;
        if( z[n+1]!=delim ) break;
      }
    }
  }else{
    int c;
    for(n=1; (c = z[n])!=0 && !isspace(c) && c!='"' && c!='\'' && c!=';'; n++){}
  }
  return n;
}

/*
** Copy a single token into a string buffer.
*/
static int extractToken(const char *zIn, int nIn, char *zOut, int nOut){
  int i;
  if( nIn<=0 ){
    zOut[0] = 0;
    return 0;
  }
  for(i=0; i<nIn && i<nOut-1 && !isspace(zIn[i]); i++){ zOut[i] = zIn[i]; }
  zOut[i] = 0;
  return i;
}

/*
** Find the number of characters up to the start of the next "--end" token.
*/
static int findEnd(const char *z, int *pnLine){
  int n = 0;
  while( z[n] && (strncmp(z+n,"--end",5) || !isspace(z[n+5])) ){
    n += tokenLength(z+n, pnLine);
  }
  return n;
}

/*
** Find the number of characters up to the first character past the
** of the next "--endif"  or "--else" token. Nested --if commands are
** also skipped.
*/
static int findEndif(const char *z, int stopAtElse, int *pnLine){
  int n = 0;
  while( z[n] ){
    int len = tokenLength(z+n, pnLine);
    if( (strncmp(z+n,"--endif",7)==0 && isspace(z[n+7]))
     || (stopAtElse && strncmp(z+n,"--else",6)==0 && isspace(z[n+6]))
    ){
      return n+len;
    }
    if( strncmp(z+n,"--if",4)==0 && isspace(z[n+4]) ){
      int skip = findEndif(z+n+len, 0, pnLine);
      n += skip + len;
    }else{
      n += len;
    }
  }
  return n;
}

/*
** Wait for a client process to complete all its tasks
*/
static void waitForClient(int iClient, int iTimeout, char *zErrPrefix){
  sqlite3_stmt *pStmt;
  int rc;
  if( iClient>0 ){
    pStmt = prepareSql(
               "SELECT 1 FROM task"
               " WHERE client=%d"
               "   AND client IN (SELECT id FROM client)"
               "  AND endtime IS NULL",
               iClient);
  }else{
    pStmt = prepareSql(
               "SELECT 1 FROM task"
               " WHERE client IN (SELECT id FROM client)"
               "   AND endtime IS NULL");
  }
  g.iTimeout = 0;
  while( ((rc = sqlite3_step(pStmt))==SQLITE_BUSY || rc==SQLITE_ROW)
    && iTimeout>0
  ){
    sqlite3_reset(pStmt);
    sqlite3_sleep(50);
    iTimeout -= 50;
  }
  sqlite3_finalize(pStmt);
  g.iTimeout = DEFAULT_TIMEOUT;
  if( rc!=SQLITE_DONE ){
    if( zErrPrefix==0 ) zErrPrefix = "";
    if( iClient>0 ){
      errorMessage("%stimeout waiting for client %d", zErrPrefix, iClient);
    }else{
      errorMessage("%stimeout waiting for all clients", zErrPrefix);
    }
  }
}

/* Maximum number of arguments to a --command */
#define MX_ARG 2

/*
** Run a script.
*/
static void runScript(
  int iClient,       /* The client number, or 0 for the master */
  int taskId,        /* The task ID for clients.  0 for master */
  char *zScript,     /* Text of the script */
  char *zFilename    /* File from which script was read. */
){
  int lineno = 1;
  int prevLine = 1;
  int ii = 0;
  int iBegin = 0;
  int n, c, j;
  int len;
  int nArg;
  String sResult;
  char zCmd[30];
  char zError[1000];
  char azArg[MX_ARG][100];

  memset(&sResult, 0, sizeof(sResult));
  stringReset(&sResult);
  while( (c = zScript[ii])!=0 ){
    prevLine = lineno;
    len = tokenLength(zScript+ii, &lineno);
    if( isspace(c) || (c=='/' && zScript[ii+1]=='*') ){
      ii += len;
      continue;
    }
    if( c!='-' || zScript[ii+1]!='-' || !isalpha(zScript[ii+2]) ){
      ii += len;
      continue;
    }

    /* Run any prior SQL before processing the new --command */
    if( ii>iBegin ){
      char *zSql = sqlite3_mprintf("%.*s", ii-iBegin, zScript+iBegin);
      evalSql(&sResult, zSql);
      sqlite3_free(zSql);
      iBegin = ii + len;
    }

    /* Parse the --command */
    if( g.iTrace>=2 ) logMessage("%.*s", len, zScript+ii);
    n = extractToken(zScript+ii+2, len-2, zCmd, sizeof(zCmd));
    for(nArg=0; n<len-2 && nArg<MX_ARG; nArg++){
      while( n<len-2 && isspace(zScript[ii+2+n]) ){ n++; }
      if( n>=len-2 ) break;
      n += extractToken(zScript+ii+2+n, len-2-n,
                        azArg[nArg], sizeof(azArg[nArg]));
    }
    for(j=nArg; j<MX_ARG; j++) azArg[j++][0] = 0;

    /*
    **  --sleep N
    **
    ** Pause for N milliseconds
    */
    if( strcmp(zCmd, "sleep")==0 ){
      sqlite3_sleep(atoi(azArg[0]));
    }else 

    /*
    **   --exit N
    **
    ** Exit this process.  If N>0 then exit without shutting down
    ** SQLite.  (In other words, simulate a crash.)
    */
    if( strcmp(zCmd, "exit")==0 ){
      int rc = atoi(azArg[0]);
      finishScript(iClient, taskId, 1);
      if( rc==0 ) sqlite3_close(g.db);
      exit(rc);
    }else

    /*
    **  --reset
    **
    ** Reset accumulated results back to an empty string
    */
    if( strcmp(zCmd, "reset")==0 ){
      stringReset(&sResult);
    }else

    /*
    **  --match ANSWER...
    **
    ** Check to see if output matches ANSWER.  Report an error if not.
    */
    if( strcmp(zCmd, "match")==0 ){
      int jj;
      char *zAns = zScript+ii;
      for(jj=7; jj<len-1 && isspace(zAns[jj]); jj++){}
      zAns += jj;
      if( strncmp(sResult.z, zAns, len-jj-1) ){
        errorMessage("line %d of %s:\nExpected [%.*s]\n     Got [%s]",
          prevLine, zFilename, len-jj-1, zAns, sResult.z);
      }
      g.nTest++;
      stringReset(&sResult);
    }else

    /*
    **  --source FILENAME
    **
    ** Run a subscript from a separate file.
    */
    if( strcmp(zCmd, "source")==0 ){
      char *zNewFile, *zNewScript;
      char *zToDel = 0;
      zNewFile = azArg[0];
      if( zNewFile[0]!='/' ){
        int k;
        for(k=(int)strlen(zFilename)-1; k>=0 && zFilename[k]!='/'; k--){}
        if( k>0 ){
          zNewFile = zToDel = sqlite3_mprintf("%.*s/%s", k,zFilename,zNewFile);
        }
      }
      zNewScript = readFile(zNewFile);
      if( g.iTrace ) logMessage("begin script [%s]\n", zNewFile);
      runScript(0, 0, zNewScript, zNewFile);
      sqlite3_free(zNewScript);
      sqlite3_free(zToDel);
      if( g.iTrace ) logMessage("end script [%s]\n", zNewFile);
    }else

    /*
    **  --print MESSAGE....
    **
    ** Output the remainder of the line to the log file
    */
    if( strcmp(zCmd, "print")==0 ){
      int jj;
      for(jj=7; jj<len && isspace(zScript[ii+jj]); jj++){}
      logMessage("%.*s", len-jj, zScript+ii+jj);
    }else

    /*
    **  --if EXPR
    **
    ** Skip forward to the next matching --endif or --else if EXPR is false.
    */
    if( strcmp(zCmd, "if")==0 ){
      int jj, rc;
      sqlite3_stmt *pStmt;
      for(jj=4; jj<len && isspace(zScript[ii+jj]); jj++){}
      pStmt = prepareSql("SELECT %.*s", len-jj, zScript+ii+jj);
      rc = sqlite3_step(pStmt);
      if( rc!=SQLITE_ROW || sqlite3_column_int(pStmt, 0)==0 ){
        ii += findEndif(zScript+ii+len, 1, &lineno);
      }
      sqlite3_finalize(pStmt);
    }else

    /*
    **  --else
    **
    ** This command can only be encountered if currently inside an --if that
    ** is true.  Skip forward to the next matching --endif.
    */
    if( strcmp(zCmd, "else")==0 ){
      ii += findEndif(zScript+ii+len, 0, &lineno);
    }else

    /*
    **  --endif
    **
    ** This command can only be encountered if currently inside an --if that
    ** is true or an --else of a false if.  This is a no-op.
    */
    if( strcmp(zCmd, "endif")==0 ){
      /* no-op */
    }else

    /*
    **  --start CLIENT
    **
    ** Start up the given client.
    */
    if( strcmp(zCmd, "start")==0 && iClient==0 ){
      int iNewClient = atoi(azArg[0]);
      if( iNewClient>0 ){
        startClient(iNewClient);
      }
    }else

    /*
    **  --wait CLIENT TIMEOUT
    **
    ** Wait until all tasks complete for the given client.  If CLIENT is
    ** "all" then wait for all clients to complete.  Wait no longer than
    ** TIMEOUT milliseconds (default 10,000)
    */
    if( strcmp(zCmd, "wait")==0 && iClient==0 ){
      int iTimeout = nArg>=2 ? atoi(azArg[1]) : 10000;
      sqlite3_snprintf(sizeof(zError),zError,"line %d of %s\n",
                       prevLine, zFilename);
      waitForClient(atoi(azArg[0]), iTimeout, zError);
    }else

    /*
    **  --task CLIENT
    **     <task-content-here>
    **  --end
    **
    ** Assign work to a client.  Start the client if it is not running
    ** already.
    */
    if( strcmp(zCmd, "task")==0 && iClient==0 ){
      int iTarget = atoi(azArg[0]);
      int iEnd;
      char *zTask;
      iEnd = findEnd(zScript+ii+len, &lineno);
      if( iTarget<0 ){
        errorMessage("line %d of %s: bad client number: %d",
                     prevLine, zFilename, iTarget);
      }else{
        zTask = sqlite3_mprintf("%.*s", iEnd, zScript+ii+len);
        startClient(iTarget);
        runSql("INSERT INTO task(client,script)"
               " VALUES(%d,'%q')", iTarget, zTask);
        sqlite3_free(zTask);
      }
      iEnd += tokenLength(zScript+ii+len+iEnd, &lineno);
      len += iEnd;
      iBegin = ii+len;
    }else

    /* error */{
      errorMessage("line %d of %s: unknown command --%s",
                   prevLine, zFilename, zCmd);
    }
    ii += len;
  }
  if( iBegin<ii ){
    char *zSql = sqlite3_mprintf("%.*s", ii-iBegin, zScript+iBegin);
    runSql(zSql);
    sqlite3_free(zSql);
  }
  stringFree(&sResult);
}

/*
** Look for a command-line option.  If present, return a pointer.
** Return NULL if missing.
**
** hasArg==0 means the option is a flag.  It is either present or not.
** hasArg==1 means the option has an argument.  Return a pointer to the
** argument.
*/
static char *findOption(
  char **azArg,
  int *pnArg,
  const char *zOption,
  int hasArg
){
  int i, j;
  char *zReturn = 0;
  int nArg = *pnArg;

  assert( hasArg==0 || hasArg==1 );
  for(i=0; i<nArg; i++){
    const char *z;
    if( i+hasArg >= nArg ) break;
    z = azArg[i];
    if( z[0]!='-' ) continue;
    z++;
    if( z[0]=='-' ){
      if( z[1]==0 ) break;
      z++;
    }
    if( strcmp(z,zOption)==0 ){
      if( hasArg && i==nArg-1 ){
        fatalError("command-line option \"--%s\" requires an argument", z);
      }
      if( hasArg ){
        zReturn = azArg[i+1];
      }else{
        zReturn = azArg[i];
      }
      j = i+1+(hasArg!=0);
      while( j<nArg ) azArg[i++] = azArg[j++];
      *pnArg = i;
      return zReturn;
    }
  }
  return zReturn;
}

/* Print a usage message for the program and exit */
static void usage(const char *argv0){
  int i;
  const char *zTail = argv0;
  for(i=0; argv0[i]; i++){
    if( argv0[i]=='/' ) zTail = argv0+i+1;
  }
  fprintf(stderr,"Usage: %s DATABASE ?OPTIONS? ?SCRIPT?\n", zTail);
  exit(1);
}

/* Report on unrecognized arguments */
static void unrecognizedArguments(
  const char *argv0,
  int nArg,
  char **azArg
){
  int i;
  fprintf(stderr,"%s: unrecognized arguments:", argv0);
  for(i=0; i<nArg; i++){
    fprintf(stderr," %s", azArg[i]);
  }
  fprintf(stderr,"\n");
  exit(1);
}

int main(int argc, char **argv){
  const char *zClient;
  int iClient;
  int n, i;
  int openFlags = SQLITE_OPEN_READWRITE;
  int rc;
  char *zScript;
  int taskId;
  const char *zTrace;
  const char *zCOption;

  g.argv0 = argv[0];
  g.iTrace = 1;
  if( argc<2 ) usage(argv[0]);
  g.zDbFile = argv[1];
  if( strglob("*.test", g.zDbFile) ) usage(argv[0]);
  if( strcmp(sqlite3_sourceid(), SQLITE_SOURCE_ID)!=0 ){
    fprintf(stderr, "SQLite library and header mismatch\n"
                    "Library: %s\n"
                    "Header:  %s\n",
                    sqlite3_sourceid(), SQLITE_SOURCE_ID);
    exit(1);
  }
  n = argc-2;
  sqlite3_snprintf(sizeof(g.zName), g.zName, "mptest");
  g.zVfs = findOption(argv+2, &n, "vfs", 1);
  zClient = findOption(argv+2, &n, "client", 1);
  g.zErrLog = findOption(argv+2, &n, "errlog", 1);
  g.zLog = findOption(argv+2, &n, "log", 1);
  zTrace = findOption(argv+2, &n, "trace", 1);
  if( zTrace ) g.iTrace = atoi(zTrace);
  if( findOption(argv+2, &n, "quiet", 0)!=0 ) g.iTrace = 0;
  g.bSqlTrace = findOption(argv+2, &n, "sqltrace", 0)!=0;
  if( g.zErrLog ){
    g.pErrLog = fopen(g.zErrLog, "a");
  }else{
    g.pErrLog = stderr;
  }
  if( g.zLog ){
    g.pLog = fopen(g.zLog, "a");
  }else{
    g.pLog = stdout;
  }
  sqlite3_config(SQLITE_CONFIG_LOG, sqlErrorCallback, 0);
  if( zClient ){
    iClient = atoi(zClient);
    if( iClient<1 ) fatalError("illegal client number: %d\n", iClient);
    sqlite3_snprintf(sizeof(g.zName), g.zName, "client%02d", iClient);
  }else{
    if( g.iTrace>0 ){
      printf("With SQLite " SQLITE_VERSION " " SQLITE_SOURCE_ID "\n" );
      for(i=0; (zCOption = sqlite3_compileoption_get(i))!=0; i++){
        printf("-DSQLITE_%s\n", zCOption);
      }
      fflush(stdout);
    }
    iClient =  0;
    unlink(g.zDbFile);
    openFlags |= SQLITE_OPEN_CREATE;
  }
  rc = sqlite3_open_v2(g.zDbFile, &g.db, openFlags, g.zVfs);
  if( rc ) fatalError("cannot open [%s]", g.zDbFile);
  sqlite3_busy_handler(g.db, busyHandler, 0);
  g.iTimeout = DEFAULT_TIMEOUT;
  if( g.bSqlTrace ) sqlite3_trace(g.db, sqlTraceCallback, 0);
  if( iClient>0 ){
    if( n>0 ) unrecognizedArguments(argv[0], n, argv+2);
    if( g.iTrace ) logMessage("start-client");
    while(1){
      char zTaskName[50];
      rc = startScript(iClient, &zScript, &taskId);
      if( rc==SQLITE_DONE ) break;
      if( g.iTrace ) logMessage("begin task %d", taskId);
      sqlite3_snprintf(sizeof(zTaskName), zTaskName, "client%02d-task-%d",
                       iClient, taskId);
      runScript(iClient, taskId, zScript, zTaskName);
      if( g.iTrace ) logMessage("end task %d", taskId);
      finishScript(iClient, taskId, 0);
      sqlite3_sleep(10);
    }
    if( g.iTrace ) logMessage("end-client");
  }else{
    sqlite3_stmt *pStmt;
    int iTimeout;
    if( n==0 ){
      fatalError("missing script filename");
    }
    if( n>1 ) unrecognizedArguments(argv[0], n, argv+2);
    runSql(
      "CREATE TABLE task(\n"
      "  id INTEGER PRIMARY KEY,\n"
      "  client INTEGER,\n"
      "  starttime DATE,\n"
      "  endtime DATE,\n"
      "  script TEXT\n"
      ");"
      "CREATE INDEX task_i1 ON task(client, starttime);\n"
      "CREATE INDEX task_i2 ON task(client, endtime);\n"
      "CREATE TABLE counters(nError,nTest);\n"
      "INSERT INTO counters VALUES(0,0);\n"
      "CREATE TABLE client(id INTEGER PRIMARY KEY, wantHalt);\n"
    );
    zScript = readFile(argv[2]);
    if( g.iTrace ) logMessage("begin script [%s]\n", argv[2]);
    runScript(0, 0, zScript, argv[2]);
    sqlite3_free(zScript);
    if( g.iTrace ) logMessage("end script [%s]\n", argv[2]);
    waitForClient(0, 2000, "during shutdown...\n");
    trySql("UPDATE client SET wantHalt=1");
    sqlite3_sleep(10);
    g.iTimeout = 0;
    iTimeout = 1000;
    while( ((rc = trySql("SELECT 1 FROM client"))==SQLITE_BUSY
        || rc==SQLITE_ROW) && iTimeout>0 ){
      sqlite3_sleep(10);
      iTimeout -= 10;
    }
    sqlite3_sleep(100);
    pStmt = prepareSql("SELECT nError, nTest FROM counters");
    iTimeout = 1000;
    while( (rc = sqlite3_step(pStmt))==SQLITE_BUSY && iTimeout>0 ){
      sqlite3_sleep(10);
      iTimeout -= 10;
    }
    if( rc==SQLITE_ROW ){
      g.nError += sqlite3_column_int(pStmt, 0);
      g.nTest += sqlite3_column_int(pStmt, 1);
    }
    sqlite3_finalize(pStmt);
  }
  sqlite3_close(g.db);  
  maybeClose(g.pLog);
  maybeClose(g.pErrLog);
  if( iClient==0 ){
    printf("Summary: %d errors in %d tests\n", g.nError, g.nTest);
  }
  return g.nError>0;
}
