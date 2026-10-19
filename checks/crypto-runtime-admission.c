/* Native session admission with the real linked producer's retained record. */
#include "crypto_session.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv)
{
 assert(argc==2);struct crypto_session session;
 FILE *f=fopen(argv[1],"rb");assert(f&&fread(&session,1,sizeof(session),f)==sizeof(session)&&fgetc(f)==EOF&&!fclose(f));
 assert(session.version==2&&crypto_session_valid(&session));
 struct crypto_session bad=session;memset(bad.reference,0,32);assert(crypto_session_valid(&bad));
 bad=session;bad.reference[0]=1;assert(crypto_session_valid(&bad));
 bad=session;memset(bad.fixture,0,32);assert(!crypto_session_valid(&bad));
 bad=session;bad.owner=bad.board.usb0_owner;assert(!crypto_session_valid(&bad));
 bad=session;bad.version=3;assert(!crypto_session_valid(&bad));
 puts("PASS runtime crypto session: real producer geometry, committed input, optional oracle and authority refusal");
}
