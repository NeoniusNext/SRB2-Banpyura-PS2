#ifdef FLAT_FREESTANDING32
typedef unsigned int size_t;
typedef unsigned int uintptr_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef int int32_t;
#define NULL ((void *)0)
static void *memset(void *p, int c, size_t n) { unsigned char *d=p; while(n--)*d++=(unsigned char)c; return p; }
static void *memcpy(void *p, const void *s, size_t n) { unsigned char *d=p; const unsigned char *a=s; while(n--)*d++=*a++; return p; }
static int memcmp(const void *p, const void *q, size_t n) { const unsigned char *a=p,*b=q; while(n--){if(*a!=*b)return (int)*a-(int)*b;a++;b++;}return 0; }
static size_t strlen(const char *p) { size_t n=0;while(p[n])n++;return n; }
static int strcmp(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return(unsigned char)*a-(unsigned char)*b;}
static int strcasecmp(const char *a,const char *b){unsigned x,y;do{x=(unsigned char)*a++;y=(unsigned char)*b++;if(x>='A'&&x<='Z')x+=32;if(y>='A'&&y<='Z')y+=32;if(x!=y)return(int)x-(int)y;}while(x);return 0;}
static int atoi(const char *s){unsigned int v=0;int sign=1;while(*s==' '||(*s>='\t'&&*s<='\r'))s++;if(*s=='-'||*s=='+'){if(*s=='-')sign=-1;s++;}while(*s>='0'&&*s<='9')v=v*10u+(unsigned)(*s++-'0');return sign<0?(int)(0u-v):(int)v;}
typedef unsigned long jmp_buf[6];
extern int setjmp(jmp_buf) __attribute__((returns_twice));
extern void longjmp(jmp_buf,int) __attribute__((noreturn));
__asm__(".global setjmp\nsetjmp:\nmovl 4(%esp),%edx\nmovl %ebx,0(%edx)\nmovl %esi,4(%edx)\nmovl %edi,8(%edx)\nmovl %ebp,12(%edx)\nleal 4(%esp),%ecx\nmovl %ecx,16(%edx)\nmovl (%esp),%ecx\nmovl %ecx,20(%edx)\nxorl %eax,%eax\nret\n.global longjmp\nlongjmp:\nmovl 4(%esp),%edx\nmovl 8(%esp),%eax\nmovl 0(%edx),%ebx\nmovl 4(%edx),%esi\nmovl 8(%edx),%edi\nmovl 12(%edx),%ebp\nmovl 16(%edx),%esp\njmp *20(%edx)\n");
static void out(const char *s){unsigned n=strlen(s);__asm__ volatile("int $0x80"::"a"(4),"b"(1),"c"(s),"d"(n):"memory");}
__attribute__((noreturn)) static void quit(int value){__asm__ volatile("int $0x80"::"a"(1),"b"(value):"memory");__builtin_unreachable();}
static int test_main(int,char **);
__asm__(".global _start\n_start:\nxorl %ebp,%ebp\npopl %eax\nmovl %esp,%edx\npushl %edx\npushl %eax\ncall test_main\nmovl %eax,%ebx\nmovl $1,%eax\nint $0x80\n");
#else
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <setjmp.h>
static void out(const char *s){fputs(s,stdout);}
__attribute__((noreturn)) static void quit(int value){exit(value);}
#endif
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef int32_t INT32;
typedef enum { false, true } boolean;
