/*
 * seed.c for 2.11BSD PDP-11. K&R C.
 * cc -o seed seed_pdp11.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define VERSION  "0.2.0"
#define DPORT    8080
#define HBUF     2048
#define RBUF     8192
#define MBODY    16384
#define MAXEV    64
#define ELEN     96

static char *IDIR = "/usr/local/seed";
static char *TFILE = "/usr/local/seed/token";
static char *SFILE = "/usr/local/seed/seed.c";
static char *BFILE = "/usr/local/seed/seed";
static char *BNEW = "/usr/local/seed/seed-new";
static char *BBAK = "/usr/local/seed/seed.bak";
static char *BLOG = "/usr/local/seed/build.log";
static char *CFILE = "/usr/local/seed/config.md";
static char *FFILE = "/usr/local/seed/apply_failures";

static int g_port;
static long g_start;
static char g_tok[65];
static char g_portstr[8];
static struct { long ts; char msg[ELEN]; } g_ev[MAXEV];
static int g_evh, g_evc;

/* === Events === */
void ev_add(msg) char *msg; {
    long now; time(&now);
    g_ev[g_evh].ts = now;
    strncpy(g_ev[g_evh].msg, msg, ELEN - 1);
    g_evh = (g_evh + 1) % MAXEV;
    if (g_evc < MAXEV) g_evc++;
    fprintf(stderr, "[ev] %s\n", msg);
}

/* === Failure counter (persisted to file) === */
int fail_read() {
    FILE *fp; int n;
    n=0; fp=fopen(FFILE,"r");
    if(fp){fscanf(fp,"%d",&n);fclose(fp);}
    return n;
}

void fail_write(n) int n; {
    FILE *fp; fp=fopen(FFILE,"w");
    if(fp){fprintf(fp,"%d\n",n);fclose(fp);}
}

void fail_reset() { unlink(FFILE); }

/* === Health check via raw socket === */
int health_check(port) int port; {
    int s,r; struct sockaddr_in a; char req[64], buf[512];
    s=socket(AF_INET,SOCK_STREAM,0);
    if(s<0) return -1;
    bzero(&a,sizeof(a));
    a.sin_family=AF_INET;
    a.sin_port=htons(port);
    a.sin_addr.s_addr=inet_addr("127.0.0.1");
    if(connect(s,&a,sizeof(a))<0){close(s);return -1;}
    sprintf(req,"GET /health HTTP/1.0\015\n\015\n");
    write(s,req,strlen(req));
    r=read(s,buf,511);
    close(s);
    if(r<=0) return -1;
    buf[r]=0;
    if(strstr(buf,"\"ok\":true")) return 0;
    return -1;
}

/* === JSON escape === */
int jesc(src, dst, max) char *src, *dst; int max; {
    int i, o; o = 0;
    for (i = 0; src[i] && o < max - 2; i++) {
        if (src[i]=='"'||src[i]=='\\') { dst[o++]='\\'; dst[o++]=src[i]; }
        else if (src[i]=='\n') { dst[o++]='\\'; dst[o++]='n'; }
        else if (src[i]=='\t') { dst[o++]='\\'; dst[o++]='t'; }
        else dst[o++] = src[i];
    }
    dst[o]='\0'; return o;
}

/* === Case-insensitive strstr === */
char *cistr(h, n) char *h, *n; {
    int i, nl; char a, b;
    nl = strlen(n);
    for (; *h; h++) {
        for (i = 0; i < nl; i++) {
            a=h[i]; b=n[i];
            if (a>='A'&&a<='Z') a+=32;
            if (b>='A'&&b<='Z') b+=32;
            if (a!=b) break;
        }
        if (i==nl) return h;
    }
    return (char *)0;
}

/* === Token === */
void tok_load() {
    FILE *fp; int i, l; unsigned long s;
    struct timeval tv; char hex[4];
    fp = fopen(TFILE, "r");
    if (fp) {
        if (fgets(g_tok, 65, fp)) {
            l = strlen(g_tok);
            while (l>0 && g_tok[l-1]<' ') g_tok[--l]='\0';
        }
        fclose(fp);
        if (g_tok[0]) { fprintf(stderr, "[auth] loaded\n"); return; }
    }
    gettimeofday(&tv, (struct timezone *)0);
    s = tv.tv_sec ^ tv.tv_usec ^ getpid();
    for (i = 0; i < 16; i++) {
        s = s * 1103515245 + 12345;
        sprintf(hex, "%02x", (unsigned)((s>>16)&0xff));
        g_tok[i*2]=hex[0]; g_tok[i*2+1]=hex[1];
    }
    g_tok[32]='\0';
    system("mkdir -p /usr/local/seed");
    fp = fopen(TFILE, "w");
    if (fp) { fprintf(fp, "%s\n", g_tok); fclose(fp); }
    chmod(TFILE, 0600);
    fprintf(stderr, "[auth] token: %s\n", g_tok);
}

/* === HTTP parser === */
struct hreq { char me[8]; char pa[256]; char au[128]; char *bo; int bl, cl; };

int parsereq(fd, r) int fd; struct hreq *r; {
    char hdr[HBUF]; int tot,n,he,al; char *e,*c,*a;
    bzero(r, sizeof(*r)); r->bo=(char*)0; tot=0; he=-1;
    while (tot < HBUF-1) {
        n = read(fd, hdr+tot, HBUF-1-tot);
        if (n<=0) return -1;
        tot+=n; hdr[tot]='\0';
        e = strstr(hdr, "\015\n\015\n");
        if (e) { he=(e-hdr)+4; break; }
    }
    if (he<0) return -1;
    if (sscanf(hdr,"%7s %255s",r->me,r->pa)!=2) return -1;
    c=cistr(hdr,"Content-Length:");
    if (c) r->cl=atoi(c+15);
    a=cistr(hdr,"Authorization: Bearer ");
    if (a) { int i; a+=22;
        for(i=0;a[i]&&a[i]!='\015'&&a[i]!='\n'&&i<127;i++) r->au[i]=a[i];
        r->au[i]='\0';
    }
    if (r->cl>0) {
        if (r->cl>MBODY) return -2;
        r->bo=(char*)malloc((unsigned)(r->cl+1));
        if (!r->bo) return -2;
        al=tot-he; if(al>r->cl)al=r->cl;
        if (al>0) bcopy(hdr+he,r->bo,al);
        r->bl=al;
        while (r->bl<r->cl) {
            n=read(fd,r->bo+r->bl,r->cl-r->bl);
            if(n<=0)break; r->bl+=n;
        }
        r->bo[r->bl]='\0';
    }
    return 0;
}

/* === HTTP response === */
void sresp(fd,st,stxt,ct,body) int fd,st; char *stxt,*ct,*body; {
    char h[384]; int bl, hl;
    bl = body ? strlen(body) : 0;
    hl = sprintf(h, "HTTP/1.0 %d %s\015\nContent-Type: %s\015\nContent-Length: %d\015\nConnection: close\015\n\015\n", st, stxt, ct, bl);
    write(fd, h, hl);
    if (body && bl>0) write(fd, body, bl);
}

void jr(fd,st,s,j) int fd,st; char *s,*j;
    { sresp(fd,st,s,"application/json",j); }
void tr(fd,st,s,t) int fd,st; char *s,*t;
    { sresp(fd,st,s,"text/plain",t); }

/* === File I/O === */
char *frd(p) char *p; {
    FILE *f; long sz; char *b;
    f=fopen(p,"r"); if(!f) return (char*)0;
    fseek(f,0L,2); sz=ftell(f); fseek(f,0L,0);
    b=(char*)malloc((unsigned)(sz+1));
    if(!b){fclose(f);return(char*)0;}
    fread(b,1,(int)sz,f); b[sz]='\0'; fclose(f);
    return b;
}

int fwr(p,d,l) char *p,*d; int l; {
    FILE *f; f=fopen(p,"w");
    if(!f) return -1;
    fwrite(d,1,l,f); fclose(f); return 0;
}

/* === Command output capture === */
char *cmdout(cmd, buf, max) char *cmd, *buf; int max; {
    FILE *fp; int l;
    buf[0]=0;
    fp=popen(cmd,"r");
    if(!fp) return buf;
    if(fgets(buf,max,fp)){
        l=strlen(buf);
        while(l>0&&(buf[l-1]=='\n'||buf[l-1]=='\r'))buf[--l]=0;
    }
    pclose(fp);
    return buf;
}

/* === Hardware discovery for /capabilities === */
int hw_discover(buf, max) char *buf; int max; {
    int o; char tmp[128]; char hn[64]; FILE *fp; char line[128];

    o=0;
    gethostname(hn,64);
    o+=sprintf(buf+o,"\"hostname\":\"%s\"",hn);
    o+=sprintf(buf+o,",\"arch\":\"PDP-11\"");
    o+=sprintf(buf+o,",\"os\":\"2.11BSD\"");
    o+=sprintf(buf+o,",\"cpu_model\":\"PDP-11\"");
    o+=sprintf(buf+o,",\"cpus\":1");
    o+=sprintf(buf+o,",\"int_bits\":16");
    o+=sprintf(buf+o,",\"mem_kb\":4096");

    /* Disk: parse df output for root and /usr */
    fp=popen("df 2>/dev/null","r");
    if(fp){
        while(fgets(line,sizeof(line),fp)){
            long used,avail; char fs[64], mp[64];
            if(sscanf(line,"%s %*d %ld %ld %*s %s",fs,&used,&avail,mp)==4){
                if(strcmp(mp,"/")==0)
                    o+=sprintf(buf+o,",\"root_used_kb\":%ld,\"root_free_kb\":%ld",used,avail);
                else if(strcmp(mp,"/usr")==0)
                    o+=sprintf(buf+o,",\"usr_used_kb\":%ld,\"usr_free_kb\":%ld",used,avail);
            }
        }
        pclose(fp);
    }

    /* Compiler */
    o+=sprintf(buf+o,",\"has_cc\":true");
    o+=sprintf(buf+o,",\"compiler\":\"cc (PCC)\"");

    /* Network interfaces */
    o+=sprintf(buf+o,",\"net_interfaces\":[");
    fp=popen("ifconfig -a 2>/dev/null | grep '^[a-z]' | awk -F: '{print $1}'","r");
    if(fp){
        int first; first=1;
        while(fgets(line,sizeof(line),fp)){
            int l; l=strlen(line);
            while(l>0&&(line[l-1]=='\n'||line[l-1]=='\r'||line[l-1]==' '))line[--l]=0;
            if(line[0]){
                if(!first)o+=sprintf(buf+o,",");
                o+=sprintf(buf+o,"\"%s\"",line);
                first=0;
            }
        }
        pclose(fp);
    }
    o+=sprintf(buf+o,"]");

    /* TTY devices */
    o+=sprintf(buf+o,",\"tty_devices\":[");
    fp=popen("ls /dev/tty0* /dev/dz* /dev/dh* 2>/dev/null","r");
    if(fp){
        int first; first=1;
        while(fgets(line,sizeof(line),fp)){
            int l; l=strlen(line);
            while(l>0&&(line[l-1]=='\n'||line[l-1]=='\r'))line[--l]=0;
            if(line[0]){
                if(!first)o+=sprintf(buf+o,",");
                o+=sprintf(buf+o,"\"%s\"",line);
                first=0;
            }
        }
        pclose(fp);
    }
    o+=sprintf(buf+o,"]");

    /* Kernel config info */
    cmdout("uname -a 2>/dev/null || echo '2.11BSD'",tmp,sizeof(tmp));
    if(tmp[0]){
        char esc[128]; jesc(tmp,esc,128);
        o+=sprintf(buf+o,",\"uname\":\"%s\"",esc);
    }

    return o;
}

/* === Request handler === */
void handle(fd, ip) int fd; char *ip; {
    struct hreq rq; int rc; char R[RBUF]; long ut;
    char eb[96]; char hn[64]; char cmd[512];

    rc=parsereq(fd,&rq);
    if(rc==-1){jr(fd,400,"Bad","{\"error\":\"bad\"}");return;}
    if(rc==-2){jr(fd,413,"Big","{\"error\":\"big\"}");return;}

    /* /health — always public */
    if (strcmp(rq.pa,"/health")==0) {
        time(&ut); ut-=g_start;
        sprintf(R,"{\"ok\":true,\"uptime_sec\":%ld,\"type\":\"seed\",\"version\":\"%s\",\"platform\":\"2.11BSD\",\"arch\":\"PDP-11\",\"node_type\":\"pdp11\"}",ut,VERSION);
        jr(fd,200,"OK",R); goto done;
    }
    /* auth check */
    if (strcmp(ip,"127.0.0.1")!=0 && g_tok[0] && strcmp(rq.au,g_tok)!=0) {
        jr(fd,401,"No","{\"error\":\"need auth\"}");
        sprintf(eb,"auth fail %s",ip); ev_add(eb); goto done;
    }
    /* /capabilities — real hardware discovery */
    if (strcmp(rq.pa,"/capabilities")==0) {
        int o; o=0;
        o+=sprintf(R+o,"{\"type\":\"seed\",\"version\":\"%s\",\"seed\":true,",VERSION);
        o+=hw_discover(R+o,RBUF-o);
        o+=sprintf(R+o,",\"endpoints\":[\"/health\",\"/capabilities\",\"/config.md\",\"/events\",\"/firmware/version\",\"/firmware/source\",\"/firmware/build\",\"/firmware/build/logs\",\"/firmware/apply\",\"/firmware/apply/reset\",\"/skill\"]}");
        jr(fd,200,"OK",R); goto done;
    }
    /* /events */
    if (strcmp(rq.pa,"/events")==0) {
        int s2,i,fi,o; char esc[128];
        s2=(g_evc<MAXEV)?0:g_evh;
        o=sprintf(R,"{\"events\":[");
        fi=1;
        for(i=0;i<g_evc&&o<RBUF-200;i++){
            int x; x=(s2+i)%MAXEV;
            jesc(g_ev[x].msg,esc,128);
            if(fi){o+=sprintf(R+o,"{\"t\":%ld,\"e\":\"%s\"}",g_ev[x].ts,esc);fi=0;}
            else o+=sprintf(R+o,",{\"t\":%ld,\"e\":\"%s\"}",g_ev[x].ts,esc);
        }
        sprintf(R+o,"],\"count\":%d}",g_evc);
        jr(fd,200,"OK",R); goto done;
    }
    /* GET /config.md */
    if(strcmp(rq.pa,"/config.md")==0&&strcmp(rq.me,"GET")==0){
        char *c; c=frd(CFILE);
        tr(fd,200,"OK",c?c:"# Seed PDP-11\n");
        if(c)free(c); goto done;
    }
    /* POST /config.md */
    if(strcmp(rq.pa,"/config.md")==0&&strcmp(rq.me,"POST")==0){
        if(!rq.bo){jr(fd,400,"Bad","{\"error\":\"empty\"}");goto done;}
        system("mkdir -p /usr/local/seed");
        if(fwr(CFILE,rq.bo,rq.bl)<0){jr(fd,500,"E","{\"error\":\"wfail\"}");goto done;}
        ev_add("config updated"); jr(fd,200,"OK","{\"ok\":true}"); goto done;
    }
    /* /firmware/version */
    if(strcmp(rq.pa,"/firmware/version")==0){
        time(&ut); ut-=g_start;
        sprintf(R,"{\"version\":\"%s\",\"uptime\":%ld,\"seed\":true,\"platform\":\"2.11BSD PDP-11\"}",VERSION,ut);
        jr(fd,200,"OK",R); goto done;
    }
    /* GET /firmware/source */
    if(strcmp(rq.pa,"/firmware/source")==0&&strcmp(rq.me,"GET")==0){
        char *s2; s2=frd(SFILE);
        if(s2){tr(fd,200,"OK",s2);free(s2);}
        else jr(fd,404,"No","{\"error\":\"nosrc\"}");
        goto done;
    }
    /* POST /firmware/source */
    if(strcmp(rq.pa,"/firmware/source")==0&&strcmp(rq.me,"POST")==0){
        if(!rq.bo){jr(fd,400,"Bad","{\"error\":\"empty\"}");goto done;}
        system("mkdir -p /usr/local/seed");
        if(fwr(SFILE,rq.bo,rq.bl)<0){jr(fd,500,"E","{\"error\":\"wfail\"}");goto done;}
        sprintf(eb,"src updated %d bytes",rq.bl); ev_add(eb);
        sprintf(R,"{\"ok\":true,\"bytes\":%d}",rq.bl);
        jr(fd,200,"OK",R); goto done;
    }
    /* POST /firmware/build */
    if(strcmp(rq.pa,"/firmware/build")==0&&strcmp(rq.me,"POST")==0){
        struct stat st2; int ex;
        if(stat(SFILE,&st2)!=0){jr(fd,400,"Bad","{\"ok\":false,\"error\":\"nosrc\"}");goto done;}
        sprintf(cmd,"cc -o %s %s > %s 2>&1",BNEW,SFILE,BLOG);
        rc=system(cmd); ex=(rc>>8)&0xff;
        if(ex==0){
            stat(BNEW,&st2); ev_add("build OK");
            sprintf(R,"{\"ok\":true,\"exit_code\":0,\"size\":%ld}",(long)st2.st_size);
        } else {
            char *lg; char esc[4096];
            ev_add("build FAIL"); lg=frd(BLOG);
            jesc(lg?lg:"?",esc,4096); if(lg)free(lg);
            sprintf(R,"{\"ok\":false,\"exit_code\":%d,\"errors\":\"%s\"}",ex,esc);
        }
        jr(fd,200,"OK",R); goto done;
    }
    /* GET /firmware/build/logs */
    if(strcmp(rq.pa,"/firmware/build/logs")==0){
        char *l; l=frd(BLOG);
        tr(fd,200,"OK",l?l:"(none)"); if(l)free(l); goto done;
    }
    /* POST /firmware/apply — swap binary + watchdog restart */
    if(strcmp(rq.pa,"/firmware/apply")==0&&strcmp(rq.me,"POST")==0){
        struct stat st2; int pid, fails;
        fails=fail_read();
        if(fails>=3){
            jr(fd,423,"Locked","{\"ok\":false,\"error\":\"apply locked after 3 failures, POST /firmware/apply/reset to unlock\"}");
            goto done;
        }
        if(stat(BNEW,&st2)!=0){jr(fd,400,"Bad","{\"ok\":false,\"error\":\"no built binary, POST /firmware/build first\"}");goto done;}
        /* backup current */
        sprintf(cmd,"cp %s %s 2>/dev/null",BFILE,BBAK); system(cmd);
        /* swap in new */
        sprintf(cmd,"cp %s %s && chmod 755 %s",BNEW,BFILE,BFILE);
        if(system(cmd)!=0){
            sprintf(cmd,"cp %s %s",BBAK,BFILE); system(cmd);
            jr(fd,500,"E","{\"ok\":false,\"error\":\"swap fail\"}"); goto done;
        }
        ev_add("apply: swapped, forking watchdog");
        jr(fd,200,"OK","{\"ok\":true,\"warning\":\"restarting with 10s watchdog\"}");
        close(fd); /* send response before restart */
        pid=fork();
        if(pid==0){
            /* watchdog child: kill parent, start new binary, health check */
            int i, child;
            for(i=3;i<64;i++)close(i);
            sleep(1);
            kill(getppid(),15); /* kill old server */
            sleep(2); /* let it die */
            /* start new binary */
            child=fork();
            if(child==0){
                execl(BFILE,"seed",g_portstr,(char*)0);
                _exit(1); /* exec failed */
            }
            sleep(10); /* grace period */
            /* health check */
            if(health_check(g_port)==0){
                fprintf(stderr,"[watchdog] health OK, new firmware confirmed\n");
                fail_reset(); unlink(BBAK);
            } else {
                /* rollback */
                int f; f=fail_read()+1; fail_write(f);
                fprintf(stderr,"[watchdog] health FAIL (%d/3), rolling back\n",f);
                kill(child,15); sleep(1);
                sprintf(cmd,"cp %s %s && chmod 755 %s",BBAK,BFILE,BFILE);
                system(cmd);
                /* start old binary */
                child=fork();
                if(child==0){
                    execl(BFILE,"seed",g_portstr,(char*)0);
                    _exit(1);
                }
            }
            _exit(0);
        }
        return; /* fd already closed */
    }
    /* POST /firmware/apply/reset — unlock after 3 failures */
    if(strcmp(rq.pa,"/firmware/apply/reset")==0&&strcmp(rq.me,"POST")==0){
        int f; f=fail_read();
        if(f==0){jr(fd,200,"OK","{\"ok\":true,\"message\":\"not locked\"}");goto done;}
        fail_reset();
        sprintf(eb,"apply lock reset (was %d)",f); ev_add(eb);
        jr(fd,200,"OK","{\"ok\":true,\"message\":\"apply unlocked\"}");
        goto done;
    }
    /* GET /skill — AI agent skill file */
    if(strcmp(rq.pa,"/skill")==0&&strcmp(rq.me,"GET")==0){
        char sk[4096]; int o;
        gethostname(hn,64);
        o=0;
        o+=sprintf(sk+o,"# Seed Node: %s\n\n",hn);
        o+=sprintf(sk+o,"PDP-11 seed node running 2.11BSD.\n\n");
        o+=sprintf(sk+o,"## Connection\n\n");
        o+=sprintf(sk+o,"```\nHost: %s:%d\n",ip,g_port);
        if(g_tok[0]) o+=sprintf(sk+o,"Token: %s\n",g_tok);
        o+=sprintf(sk+o,"```\n\n");
        o+=sprintf(sk+o,"All requests except /health require:\n");
        o+=sprintf(sk+o,"`Authorization: Bearer %s`\n\n",g_tok);
        o+=sprintf(sk+o,"## Endpoints\n\n");
        o+=sprintf(sk+o,"| Method | Path | Description |\n");
        o+=sprintf(sk+o,"|--------|------|-------------|\n");
        o+=sprintf(sk+o,"| GET | /health | Alive check (no auth) |\n");
        o+=sprintf(sk+o,"| GET | /capabilities | Hardware + disk + network |\n");
        o+=sprintf(sk+o,"| GET | /config.md | Node config (markdown) |\n");
        o+=sprintf(sk+o,"| POST | /config.md | Update config |\n");
        o+=sprintf(sk+o,"| GET | /events | Event log |\n");
        o+=sprintf(sk+o,"| GET | /firmware/version | Version + uptime |\n");
        o+=sprintf(sk+o,"| GET | /firmware/source | Read current source |\n");
        o+=sprintf(sk+o,"| POST | /firmware/source | Upload new C source |\n");
        o+=sprintf(sk+o,"| POST | /firmware/build | Compile (cc) |\n");
        o+=sprintf(sk+o,"| GET | /firmware/build/logs | Compiler output |\n");
        o+=sprintf(sk+o,"| POST | /firmware/apply | Hot-swap + 10s watchdog |\n");
        o+=sprintf(sk+o,"| POST | /firmware/apply/reset | Unlock after 3 fails |\n");
        o+=sprintf(sk+o,"| GET | /skill | This file |\n\n");
        o+=sprintf(sk+o,"## Constraints\n\n");
        o+=sprintf(sk+o,"- **K&R C only.** No ANSI C. No void params, no const.\n");
        o+=sprintf(sk+o,"- **cc (PCC), not gcc.** Compile: `cc -o seed seed.c`\n");
        o+=sprintf(sk+o,"- **PDP-11 16-bit.** int = 16 bits. long = 32 bits.\n");
        o+=sprintf(sk+o,"- **No snprintf.** Use sprintf carefully.\n");
        o+=sprintf(sk+o,"- **No /proc, /sys.** This is BSD, not Linux.\n");
        o+=sprintf(sk+o,"- **Single-threaded.** One request at a time.\n");
        o+=sprintf(sk+o,"- **Max body: 16KB.** Chunk larger uploads.\n");
        o+=sprintf(sk+o,"- **HTTP/1.0 only.** Use `curl -0`.\n\n");
        o+=sprintf(sk+o,"## Firmware update flow\n\n");
        o+=sprintf(sk+o,"1. `GET /firmware/source` — read current code\n");
        o+=sprintf(sk+o,"2. Write new K&R C firmware (keep all endpoints!)\n");
        o+=sprintf(sk+o,"3. `POST /firmware/source` — upload C source\n");
        o+=sprintf(sk+o,"4. `POST /firmware/build` — compile on the node\n");
        o+=sprintf(sk+o,"5. `GET /firmware/build/logs` — check for errors\n");
        o+=sprintf(sk+o,"6. `POST /firmware/apply` — swap + auto-restart\n");
        o+=sprintf(sk+o,"   Watchdog checks /health after 10s.\n");
        o+=sprintf(sk+o,"   Rolls back automatically on failure.\n");
        o+=sprintf(sk+o,"   After 3 consecutive failures, apply locks.\n");
        o+=sprintf(sk+o,"   Unlock: `POST /firmware/apply/reset`\n");
        sresp(fd,200,"OK","text/markdown",sk);
        goto done;
    }
    /* 404 */
    sprintf(R,"{\"error\":\"not found\",\"path\":\"%s\",\"hint\":\"GET /capabilities for API list\"}",rq.pa);
    jr(fd,404,"No",R);
done:
    if(rq.bo)free(rq.bo);
}

/* === Main === */
main(argc,argv) int argc; char **argv; {
    int port,srv,opt,cl; struct sockaddr_in ad,ca; int clen; char *ip; char eb[64];
    port=DPORT; if(argc>1) port=atoi(argv[1]);
    g_port=port;
    sprintf(g_portstr,"%d",port);
    signal(SIGPIPE,SIG_IGN); signal(SIGCHLD,SIG_IGN); time(&g_start);
    system("mkdir -p /usr/local/seed");
    tok_load();
    srv=socket(AF_INET,SOCK_STREAM,0);
    if(srv<0){perror("socket");exit(1);}
    opt=1; setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,(char*)&opt,sizeof(opt));
    bzero(&ad,sizeof(ad)); ad.sin_family=AF_INET;
    ad.sin_addr.s_addr=INADDR_ANY; ad.sin_port=htons(port);
    if(bind(srv,(struct sockaddr*)&ad,sizeof(ad))<0){perror("bind");exit(1);}
    if(listen(srv,5)<0){perror("listen");exit(1);}
    fprintf(stderr,"\n  Seed v%s (2.11BSD PDP-11)\n",VERSION);
    fprintf(stderr,"  Port: %d  Token: %.8s...\n\n",port,g_tok);
    sprintf(eb,"seed started port %d",port); ev_add(eb);
    for(;;){
        clen=sizeof(ca); cl=accept(srv,(struct sockaddr*)&ca,&clen);
        if(cl<0)continue; ip=inet_ntoa(ca.sin_addr);
        handle(cl,ip); close(cl);
    }
}
