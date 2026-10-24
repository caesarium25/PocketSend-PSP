"""Compile the actual HTTP parser/decoder from network.c against fragmented
in-memory input. No networking or PSP SDK is needed. Pass a Zig executable.
This tests the production function bodies, not a reimplementation.
"""
import pathlib
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[1]
src = (root/'src/network.c').read_text()
type_start = src.index('typedef struct {\n  char method')
type_end = src.index('} Http;', type_start)+len('} Http;')
start = src.index('static int headers(')
end = src.index('static int response(', start)
prefix = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include "core.h"
#define BODY_CAP (64*1024)
static const char *input;
static size_t cursor, input_size;
static unsigned long ticks;
static int checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static uint64_t now_ms(void){return ticks++;}
static void feed(const char *s){input=s;cursor=0;input_size=strlen(s);ticks=0;}
static int read_some(int fd,void *out,size_t n,uint64_t timeout){
 (void)fd;(void)timeout;
 if(cursor==input_size)return 0;
 if(n>3)n=3;
 if(n>input_size-cursor)n=input_size-cursor;
 memcpy(out,input+cursor,n);cursor+=n;return n;
}
static int read_exact(int fd,void *out,size_t n,uint64_t timeout){
 char *p=out;while(n){int r=read_some(fd,p,n,timeout);if(r<=0)return 0;p+=r;n-=r;}return 1;
}
'''
tests = r'''
int main(void){
 Http h;
 feed("POST /api/localsend/v2/upload HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc");
 CHECK(headers(0,&h,15000));CHECK(h.length==3&&!h.chunked);CHECK(!strcmp(h.method,"POST"));
 Body b=body_start(0,&h);char out[32]={0};CHECK(body_read(&b,out,sizeof out)==3&&!strcmp(out,"abc"));CHECK(body_read(&b,out,sizeof out)==0);
 feed("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}");CHECK(headers(0,&h,15000)&&h.status==200);cJSON *j=read_json(0,&h);CHECK(j!=NULL);cJSON_Delete(j);
 feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\n{\r\n1;ext=ok\r\n}\r\n0\r\nX-Trailer: ok\r\n\r\n");
 CHECK(headers(0,&h,15000)&&h.chunked);j=read_json(0,&h);CHECK(j!=NULL);cJSON_Delete(j);
 feed("POST /x HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx");CHECK(!headers(0,&h,15000));
 feed("POST /x HTTP/1.1\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n");CHECK(!headers(0,&h,15000));
 feed("POST /x HTTP/1.1\r\nContent-Length: -1\r\n\r\n");CHECK(!headers(0,&h,15000));
 feed("POST /x HTTP/1.1\r\nContent-Length: 18446744073709551616\r\n\r\n");CHECK(!headers(0,&h,15000));
 feed("POST /x HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n");CHECK(!headers(0,&h,15000));
 feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}XX0\r\n\r\n");CHECK(headers(0,&h,15000));CHECK(!read_json(0,&h));
 feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n");CHECK(headers(0,&h,15000));CHECK(!read_json(0,&h));
 feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\n{}\r\n0\r\n\r\n");CHECK(headers(0,&h,15000));CHECK(!read_json(0,&h));
 feed("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n{}");CHECK(headers(0,&h,15000));CHECK(!read_json(0,&h));
 feed("HTTP/1.1 200 OK\r\nContent-Length: 65537\r\n\r\n{}");CHECK(headers(0,&h,15000));CHECK(!read_json(0,&h));
 feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");CHECK(headers(0,&h,15000));b=body_start(0,&h);CHECK(body_read(&b,out,sizeof out)==0&&b.done);
 printf("PASS: %d HTTP checks (fragmented input, framing, chunked JSON, truncation)\n",checks);
 return 0;
}
'''
build = root/'build'
build.mkdir(exist_ok=True)
generated = build/'test_http_generated.c'
generated.write_text(prefix+src[type_start:type_end]+'\n'+src[start:end]+tests)
exe = build/'test_http.exe'
subprocess.run([sys.argv[1], 'cc', '-std=gnu99', '-Wall', '-Wextra',
                '-I'+str(root/'src'), str(generated), str(root/'src/cJSON.c'),
                '-o',str(exe)], check=True)
subprocess.run([str(exe)],check=True)
