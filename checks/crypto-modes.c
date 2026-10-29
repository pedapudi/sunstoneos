/* Application orchestration over the existing real CE service/controller and
 * its accepted descriptor-engine model. No physical authority is inferred. */
#define main archived_service_modes_main
#include "a733-ce-modes.c"
#undef main
#include "../apps/crypto/run.h"
#include "a733_storage_digest.h"
static int eligible(void *p){return *(unsigned *)p==1;}
static size_t read_file(const char *path,void *out,size_t capacity)
{FILE *f=fopen(path,"rb");assert(f);size_t n=fread(out,1,capacity,f);assert(!ferror(f)&&feof(f));fclose(f);return n;}
int main(int argc,char **argv)
{
 assert(argc==6);FILE *f=fopen(argv[1],"r");assert(f);char line[4000];tables();
 while(fgets(line,sizeof(line),f)){
  char *at=line;
  if(!strncmp(at,"k512 ",5)){at+=5;for(unsigned i=0;i<80;++i)k512[i]=strtoull(at,&at,16);}
  else if(!strncmp(at,"initial ",8)){at+=8;unsigned method=(unsigned)strtoul(at,&at,10);for(unsigned i=0;i<8;++i)initial[method][i]=strtoull(at,&at,16);}
 }fclose(f);
 static struct model m;static struct crypto_run r;
 setup(&m);assert(a733_ce_hash_bind(&r.hash,&m.session)&&a733_ce_cipher_bind(&r.cipher,&m.session));
 uint8_t raw[CRYPTO_REQUEST_MAX+1],expected[2049];size_t bytes=read_file(argv[2],raw,sizeof(raw));
 struct crypto_session session={.version=2};storage_digest_bytes(raw,bytes,session.fixture);
 if(!strcmp(argv[5],"parse-refused")){
  assert(!crypto_request_copy(&r.request,&session,raw,bytes));zeroed(r.request.secret,128);zeroed(r.request.key,16);
  puts("PASS authenticated malformed extended request refused before CE work");return 0;
 }
 assert(crypto_request_copy(&r.request,&session,raw,bytes));r.runtime=1;
 unsigned refused=!strcmp(argv[5],"auth-failure");
 int result=0;unsigned polls=0;
 for(;polls<1000&&!result;++polls){if(m.pending)complete(&m);result=crypto_run_step(&r,10000);++m.now;}
 atomic_uint published=0;unsigned valid=1;uint8_t output[2048],digest[32];memset(output,0xa5,sizeof(output));
 if(refused){
  assert(result==-1&&r.stage!=8&&!r.output_bytes);
  assert(crypto_run_publish(&r,output,digest,&published,eligible,&valid)==-1&&atomic_load(&published)==2);
  unchanged(output,sizeof(output));zeroed(r.request.secret,128);zeroed(r.cipher.key,32);zeroed(r.hash.key,128);
  puts("PASS application tag rejection publishes no output and clears CPU secrets");return 0;
 }
 assert(result==1&&r.stage==8);size_t n=read_file(argv[3],expected,sizeof(expected));assert(n==r.output_bytes&&!memcmp(expected,r.payload,n));
 assert(crypto_run_publish(&r,output,digest,&published,eligible,&valid)==1&&atomic_load(&published)==1&&!memcmp(output,expected,n));
 zeroed(r.request.secret,128);zeroed(r.request.key,16);
 f=fopen(argv[4],"wb");assert(f);assert(fwrite(output,1,n,f)==n);fclose(f);
 valid=0;assert(crypto_run_publish(&r,output,digest,&published,eligible,&valid)==-1&&atomic_load(&published)==2);
 crypto_run_clear_secrets(&r);zeroed(r.hash.key,128);zeroed(r.hash.buffer,sizeof(r.hash.buffer));zeroed(r.cipher.key,32);
 printf("PASS application mode %u method %u cipher %u: %zu output bytes, %u polls, %u hardware tasks\n",r.request.operation,r.request.method,r.request.mode,n,polls,m.starts);
}
