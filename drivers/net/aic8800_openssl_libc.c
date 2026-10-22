/* C library functions that the pinned OpenSSL library and sae.c reference
 * and the freestanding radio images do not otherwise define. There is no file system,
 * environment, user identity, process or wall clock. OpenSSL allocates through
 * the functions set by aic8800_openssl_init; the C allocator here refuses, so
 * a library use before that setup fails instead of reaching another heap.
 * Most string and sorting users are encoder, PEM and configuration code that
 * station SAE and WPA2 never call; the implementations are still complete. */
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
_Noreturn void aic8800_supplicant_abort(void);
typedef struct sunstone_openssl_dir DIR;
struct dirent;
void *malloc(size_t n){(void)n;errno=ENOMEM;return 0;}
void *realloc(void *p,size_t n){(void)p;(void)n;errno=ENOMEM;return 0;}
/* Only pointers from malloc, which never returns one, can arrive here. */
void free(void *p){(void)p;}
int posix_memalign(void **p,size_t alignment,size_t n){(void)p;(void)alignment;(void)n;return ENOMEM;}
/* OPENSSL_die faults the entered supplicant and traps like its own aborts. */
void abort(void){aic8800_supplicant_abort();}
static int error_number;
int *__errno(void){return &error_number;}
char *getenv(const char *name){(void)name;return 0;}
uid_t getuid(void){return 0;}
uid_t geteuid(void){return 0;}
gid_t getgid(void){return 0;}
gid_t getegid(void){return 0;}
/* The process identity only detects a fork, which cannot happen here. */
pid_t getpid(void){return 1;}
/* Without a wall clock the DRBG never reseeds by age; its reseed after a
 * fixed number of requests remains. */
time_t time(time_t *t){if(t)*t=(time_t)-1;return (time_t)-1;}
int gettimeofday(struct timeval *restrict tv,void *restrict tz){(void)tv;(void)tz;errno=ENOSYS;return -1;}
int stat(const char *restrict path,struct stat *restrict st){(void)path;(void)st;errno=ENOENT;return -1;}
DIR *opendir(const char *path){(void)path;errno=ENOENT;return 0;}
struct dirent *readdir(DIR *d){(void)d;return 0;}
int closedir(DIR *d){(void)d;errno=EBADF;return -1;}
int abs(int v){return v<0?-v:v;}
void *memchr(const void *s,int c,size_t n)
{const unsigned char *p=s;for(;n;--n,++p)if(*p==(unsigned char)c)return (void *)p;return 0;}
int strncmp(const char *a,const char *b,size_t n)
{for(;n;--n,++a,++b){if(*a!=*b)return (unsigned char)*a-(unsigned char)*b;if(!*a)break;}return 0;}
char *strrchr(const char *s,int c)
{const char *last=0;do{if(*s==(char)c)last=s;}while(*s++);return (char *)last;}
size_t strspn(const char *s,const char *accept)
{size_t n=0;for(;s[n]&&strchr(accept,s[n]);++n){}return n;}
size_t strcspn(const char *s,const char *reject)
{size_t n=0;for(;s[n]&&!strchr(reject,s[n]);++n){}return n;}
static int digit(int c,int base)
{
    int v=c>='0'&&c<='9'?c-'0':c>='a'&&c<='z'?c-'a'+10:c>='A'&&c<='Z'?c-'A'+10:99;
    return v<base?v:-1;
}
unsigned long strtoul(const char *restrict s,char **restrict end,int base)
{
    const char *p=s;int negative=0,overflow=0,any=0;unsigned long value=0;
    while(*p==' '||(*p>='\t'&&*p<='\r'))++p;
    if(*p=='+'||*p=='-')negative=*p++=='-';
    if((!base||base==16)&&p[0]=='0'&&(p[1]=='x'||p[1]=='X')&&digit(p[2],16)>=0){p+=2;base=16;}
    else if(!base)base=*p=='0'?8:10;
    if(base<2||base>36){if(end)*end=(char *)s;errno=EINVAL;return 0;}
    for(int d;(d=digit(*p,base))>=0;++p,any=1){
        if(value>(ULONG_MAX-(unsigned long)d)/(unsigned long)base)overflow=1;
        else value=value*(unsigned long)base+(unsigned long)d;
    }
    if(end)*end=(char *)(any?p:s);
    if(overflow){errno=ERANGE;return ULONG_MAX;}
    return negative?-value:value;
}
int atoi(const char *s)
{
    while(*s==' '||(*s>='\t'&&*s<='\r'))++s;
    int negative=*s=='-';if(*s=='+'||*s=='-')++s;
    long value=0;for(;*s>='0'&&*s<='9'&&value<=INT_MAX;++s)value=value*10+(*s-'0');
    if(value>INT_MAX)value=negative?-(long)INT_MIN:INT_MAX;
    return (int)(negative?-value:value);
}
/* Insertion sort: OpenSSL sorts only short stacks. */
void qsort(void *base,size_t count,size_t size,int (*compare)(const void *,const void *))
{
    unsigned char *a=base;
    for(size_t i=1;i<count;++i)
        for(size_t j=i;j&&compare(a+(j-1)*size,a+j*size)>0;--j)
            for(size_t k=0;k<size;++k){unsigned char t=a[(j-1)*size+k];a[(j-1)*size+k]=a[j*size+k];a[j*size+k]=t;}
}
/* The C locale table of the newlib ctype macros; entry 0 is EOF. */
#define U 01
#define L 02
#define N 04
#define S 010
#define P 020
#define C 040
#define X 0100
#define B 0200
const char _ctype_[1+256]={
    [1+0x00 ... 1+0x08]=C,[1+0x09 ... 1+0x0d]=C|S,[1+0x0e ... 1+0x1f]=C,[1+0x20]=S|B,
    [1+0x21 ... 1+0x2f]=P,[1+0x30 ... 1+0x39]=N,[1+0x3a ... 1+0x40]=P,[1+0x41 ... 1+0x46]=U|X,
    [1+0x47 ... 1+0x5a]=U,[1+0x5b ... 1+0x60]=P,[1+0x61 ... 1+0x66]=L|X,[1+0x67 ... 1+0x7a]=L,
    [1+0x7b ... 1+0x7e]=P,[1+0x7f]=C};
