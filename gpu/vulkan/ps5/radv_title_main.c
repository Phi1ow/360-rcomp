// R-comp's small RADV bring-up title. Actual compiled TLS and Vulkan functions
// execute on real threads; the owning console runner performs title shutdown.
/* The PS5 title id of this application (the build passes -DRCOMP_TITLE_ID; GTA IV is the default). */
#ifndef RCOMP_TITLE_ID
#define RCOMP_TITLE_ID "PPSA88360"
#endif
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#ifndef RCOMP_RADV_BUILD_ID
#error RCOMP_RADV_BUILD_ID is required
#endif
int rcomp_radv_test_main(int argc,char** argv);

/* Writes to the title folder cost ~30 ms each on the console and the recompiled game prints
   hundreds of debug lines, so both streams are fully buffered and a flusher thread writes the
   collected bytes to the files a few times per second. */
static void* log_flusher_main(void* argument) {
    (void)argument;
    for (;;) {
        struct timespec pause={0,250*1000*1000};nanosleep(&pause,NULL);
        fflush(stdout);fflush(stderr);
    }
    return NULL;
}
static void start_log_flusher(void) {
    static char out_buffer[1<<19], err_buffer[1<<19];
    setvbuf(stdout,out_buffer,_IOFBF,sizeof out_buffer);
    setvbuf(stderr,err_buffer,_IOFBF,sizeof err_buffer);
    pthread_t thread;
    if (pthread_create(&thread,NULL,log_flusher_main,NULL)==0) pthread_detach(thread);
}
#if defined(RCOMP_RADV_GAME)
/* Crash report: the console only says "signal 11"; print what the faulting thread was running. */
extern __thread const char* rcomp_t_fn;
extern __thread const char* rcomp_t_ring[256];
extern __thread unsigned rcomp_t_idx;
extern unsigned rcomp_debug_read_guest_u32(unsigned address);
extern void rcomp_debug_crash_context(void);
static volatile sig_atomic_t crash_handler_entered;
static void report_null_call_stack(const ucontext_t* uc) {
    const uintptr_t rsp=(uintptr_t)uc->uc_mcontext.mc_rsp;
    if (uc->uc_mcontext.mc_rip != 0 || rsp < 0x10000u ||
        (rsp & (sizeof(uintptr_t)-1)) || rsp > UINT64_C(0x00007FFFFFFFFFFF)-7*sizeof(uintptr_t)) return;
    /* A null indirect CALL leaves its return address at RSP. The context gives
       us an address, not proof that it is readable: a nested fault exits via
       the guard below. Preserve the headers and each address before reading
       the next word. These are candidate host addresses, not a stack unwind. */
    fflush(stdout);fflush(stderr);
    const volatile uintptr_t* stack=(const volatile uintptr_t*)rsp;
    for (unsigned i=0;i<8;++i) {
        const uintptr_t address=stack[i];
        if (i && (address < 0x10000u || address > UINT64_C(0x00007FFFFFFFFFFF))) continue;
        fprintf(stderr,"RCOMP-CRASH host_stack[%u]%s=0x%llx\n",i,i ? " candidate" : " call_null_return",
                (unsigned long long)address);
        fflush(stderr);
    }
}
static void crash_handler(int signo, siginfo_t* info, void* context) {
    if (crash_handler_entered) _exit(128+signo);
    crash_handler_entered=1;
    ucontext_t* uc=(ucontext_t*)context;
    fprintf(stderr,"RCOMP-CRASH signal=%d addr=%p rip=0x%llx rsp=0x%llx fn=%s\n",signo,info?info->si_addr:NULL,
            (unsigned long long)uc->uc_mcontext.mc_rip,(unsigned long long)uc->uc_mcontext.mc_rsp,rcomp_t_fn?rcomp_t_fn:"?");
    rcomp_debug_crash_context();
    fprintf(stderr,"RCOMP-CRASH log_counter[0x831373D0]=0x%08X\n",rcomp_debug_read_guest_u32(0x831373D0u));
    for (unsigned i=0;i<24;++i) {
        const char* f=rcomp_t_ring[(rcomp_t_idx-1-i)&255];
        fprintf(stderr,"RCOMP-CRASH ring[-%u]=%s\n",i,f?f:"?");
    }
    report_null_call_stack(uc);
    fflush(stdout);fflush(stderr);
    signal(signo,SIG_DFL);
    raise(signo);
}
extern void rcomp_set_fatal_hook(void (*hook)(int,const char*));
static void fatal_hook(int kind,const char* message) {
    (void)kind;(void)message;
    fprintf(stderr,"RCOMP-FATAL-CONTEXT fn=%s\n",rcomp_t_fn?rcomp_t_fn:"?");
    for (unsigned i=0;i<24;++i) {const char* f=rcomp_t_ring[(rcomp_t_idx-1-i)&255];fprintf(stderr,"RCOMP-FATAL-CONTEXT ring[-%u]=%s\n",i,f?f:"?");}
    fflush(stderr);
}
static void install_crash_handler(void) {
    rcomp_set_fatal_hook((void(*)(int,const char*))fatal_hook);
    struct sigaction action={0};
    action.sa_sigaction=crash_handler;sigemptyset(&action.sa_mask);action.sa_flags=SA_SIGINFO|SA_NODEFER;
    sigaction(SIGSEGV,&action,NULL);sigaction(SIGABRT,&action,NULL);sigaction(SIGBUS,&action,NULL);sigaction(SIGILL,&action,NULL);sigaction(SIGFPE,&action,NULL);
}
#else
static void install_crash_handler(void) {}
#endif
static _Thread_local uint64_t tls_value=0x1122334455667788ull;
static void* tls_worker(void* pointer) {
    int* result=pointer;
    *result=tls_value==0x1122334455667788ull;
    tls_value=0xFFEEDDCCBBAA0099ull;
    return NULL;
}
static void* test_worker(void* pointer) {
    int* status=pointer;
    tls_value=0x99;
    int child_ok=0;
    pthread_t child;
    for (unsigned iteration=0;iteration<2;++iteration) {
        child_ok=0;
        if (pthread_create(&child,NULL,tls_worker,&child_ok) != 0) { *status=2;return NULL; }
        if (pthread_join(child,NULL) != 0 || !child_ok || tls_value!=0x99) { *status=2;return NULL; }
    }
    printf("RCOMP-RADV tls_per_thread=PASS\n");
#if defined(RCOMP_RADV_GAME)
    // Exploratory GTA IV boot: the recompiled title runs on this worker thread.
    *status=rcomp_radv_test_main(0,NULL);
#elif defined(RCOMP_RADV_PROBE)
    char* args[]={"rcomp_radv_probe","/app0/radv_probe.json",NULL};
    *status=rcomp_radv_test_main(2,args);
#else
    char* args[]={"rcomp_radv_draw",NULL};
    *status=rcomp_radv_test_main(1,args);
#endif
    return NULL;
}
int main(int argc,char** argv) {
    (void)argc;(void)argv;
    /* Two streams opened on one file keep separate offsets and overwrite each
       other on this filesystem, so stderr gets its own file. */
    if (!freopen("/app0/rcomp_title.log","a",stdout)) return 2;
    if (!freopen("/app0/rcomp_title.err","w",stderr)) return 2;
    start_log_flusher();
    install_crash_handler();
    printf("RCOMP-TITLE begin title=%s build=%s backend=R-comp-public-RADV\n",RCOMP_TITLE_ID,RCOMP_RADV_BUILD_ID);
    pthread_attr_t attributes;
    int status=2;
    if (!pthread_attr_init(&attributes)) {
        #if defined(RCOMP_RADV_GAME)
        if (!pthread_attr_setstacksize(&attributes,32u*1024u*1024u)) {
#else
        if (!pthread_attr_setstacksize(&attributes,4u*1024u*1024u)) {
#endif
            pthread_t worker;
            const int started=pthread_create(&worker,&attributes,test_worker,&status);
            pthread_attr_destroy(&attributes);
            if (!started) pthread_join(worker,NULL);
        } else pthread_attr_destroy(&attributes);
    }
    printf("RCOMP-TITLE end status=%d\n",status);
    fflush(stdout);fflush(stderr);
    for (;;) { struct timespec delay={1,0};nanosleep(&delay,NULL); }
}
