#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <pxe/sys.h>
#include <pxe/thread.h>

/* =========================================================================
 * errno & stdio stubs
 * ========================================================================= */

static int g_vplayer_errno = 0;
int* __vplayer_errno_location(void) { return &g_vplayer_errno; }

static FILE g_stdin_obj, g_stdout_obj, g_stderr_obj;
FILE* stdin  = &g_stdin_obj;
FILE* stdout = &g_stdout_obj;
FILE* stderr = &g_stderr_obj;

FILE* fopen(const char* path, const char* mode) {
    (void)path; (void)mode;
    return NULL;
}
int fclose(FILE* stream) { (void)stream; return 0; }
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    (void)ptr; (void)size; (void)nmemb; (void)stream;
    return 0;
}
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
    (void)ptr; (void)size; (void)stream;
    return nmemb;
}
int fseek(FILE* stream, long offset, int whence) {
    (void)stream; (void)offset; (void)whence;
    return -1;
}
long ftell(FILE* stream) { (void)stream; return -1; }
int fflush(FILE* stream) { (void)stream; return 0; }

int printf(const char* fmt, ...) { (void)fmt; return 0; }
int fprintf(FILE* stream, const char* fmt, ...) { (void)stream; (void)fmt; return 0; }

int sprintf(char* buf, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = vsnprintf(buf, 4096, fmt, ap);
    va_end(ap);
    return rc;
}

int vsprintf(char* buf, const char* fmt, va_list ap) {
    return vsnprintf(buf, 4096, fmt, ap);
}

int sscanf(const char* str, const char* fmt, ...) {
    (void)str; (void)fmt;
    return 0;
}

/* =========================================================================
 * stdlib / string helpers
 * ========================================================================= */

void exit(int status) {
    pxe_exit(status);
}

void abort(void) {
    pxe_exit(-1);
}

static void swap_bytes(uint8_t* a, uint8_t* b, size_t size) {
    while (size--) {
        uint8_t t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

void qsort(void* base, size_t nmemb, size_t size,
           int (*compar)(const void*, const void*)) {
    if (!base || nmemb < 2 || size == 0 || !compar) return;
    uint8_t* arr = (uint8_t*)base;
    /* Shell sort: O(n log^2 n), non-recursive, zero stack risk */
    for (size_t gap = nmemb / 2; gap > 0; gap /= 2) {
        for (size_t i = gap; i < nmemb; i++) {
            for (size_t j = i; j >= gap; j -= gap) {
                uint8_t* p1 = arr + (j - gap) * size;
                uint8_t* p2 = arr + j * size;
                if (compar(p1, p2) > 0) {
                    swap_bytes(p1, p2, size);
                } else {
                    break;
                }
            }
        }
    }
}

long strtol(const char* nptr, char** endptr, int base) {
    if (!nptr) {
        if (endptr) *endptr = NULL;
        return 0;
    }
    const char* p = nptr;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') { p++; }

    if (base == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
        else if (p[0] == '0') { base = 8; p++; }
        else { base = 10; }
    } else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    long val = 0;
    while (*p) {
        int d = -1;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        if (d < 0 || d >= base) break;
        val = val * base + d;
        p++;
    }
    if (endptr) *endptr = (char*)p;
    return neg ? -val : val;
}

int atoi(const char* s) { return (int)strtol(s, NULL, 10); }
long atol(const char* s) { return strtol(s, NULL, 10); }

char* strstr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return NULL;
    if (!*needle) return (char*)haystack;
    for (const char* c = haystack; *c; c++) {
        const char* a = c;
        const char* b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char*)c;
    }
    return NULL;
}

char* strdup(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* copy = (char*)malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

/* =========================================================================
 * Time / sleep / CPU count
 * ========================================================================= */

int gettimeofday(struct timeval* tv, void* tz) {
    (void)tz;
    if (!tv) return -1;
    uint64_t ns = pxe_get_time_ns();
    tv->tv_sec  = (time_t)(ns / 1000000000ULL);
    tv->tv_usec = (suseconds_t)((ns % 1000000000ULL) / 1000ULL);
    return 0;
}

int clock_gettime(int clk_id, struct timespec* tp) {
    (void)clk_id;
    if (!tp) return -1;
    uint64_t ns = pxe_get_time_ns();
    tp->tv_sec  = (time_t)(ns / 1000000000ULL);
    tp->tv_nsec = (long)(ns % 1000000000ULL);
    return 0;
}

time_t time(time_t* tloc) {
    time_t t = (time_t)(pxe_get_time_ns() / 1000000000ULL);
    if (tloc) *tloc = t;
    return t;
}

static struct tm g_tm_zero;
struct tm* localtime(const time_t* timer) {
    (void)timer;
    memset(&g_tm_zero, 0, sizeof(g_tm_zero));
    return &g_tm_zero;
}

size_t strftime(char* s, size_t maxsize, const char* fmt, const struct tm* tm) {
    (void)fmt; (void)tm;
    if (s && maxsize > 0) s[0] = '\0';
    return 0;
}

int usleep(useconds_t usec) {
    if (usec > 0) pxe_sleep_us((uint64_t)usec);
    return 0;
}

unsigned int sleep(unsigned int seconds) {
    if (seconds > 0) pxe_sleep_ms(seconds * 1000u);
    return 0;
}

static int detect_logical_cpus(void) {
    /* Query CPUID leaf 0xB / 0x1 to estimate logical CPU count safely in Ring 3 */
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    uint32_t max_leaf = eax;
    int ncpu = 1;
    if (max_leaf >= 0x0B) {
        __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x0B), "c"(1));
        if ((ebx & 0xFFFFu) > 0) ncpu = (int)(ebx & 0xFFFFu);
    }
    if (ncpu <= 1 && max_leaf >= 1) {
        __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
        if (edx & (1u << 28)) {
            int log_cnt = (int)((ebx >> 16) & 0xFFu);
            if (log_cnt > 1) ncpu = log_cnt;
        }
    }
    if (ncpu < 1) ncpu = 1;
    if (ncpu > 16) ncpu = 16; /* OpenH264 caps decoder threads at 16 */
    return ncpu;
}

long sysconf(int name) {
    if (name == _SC_NPROCESSORS_ONLN) return (long)detect_logical_cpus();
    return -1;
}

int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t* mask) {
    (void)pid;
    if (!mask || cpusetsize < sizeof(cpu_set_t)) return -1;
    CPU_ZERO(mask);
    int ncpu = detect_logical_cpus();
    for (int i = 0; i < ncpu && i < CPU_SETSIZE; i++) {
        CPU_SET(i, mask);
    }
    return 0;
}

int sched_yield(void) {
    pxe_yield();
    return 0;
}

/* =========================================================================
 * POSIX pthread & semaphore shims backed by <pxe/thread.h>
 * ========================================================================= */

int pthread_attr_init(pthread_attr_t* attr) {
    if (!attr) return EINVAL;
    attr->policy = SCHED_OTHER;
    attr->scope = PTHREAD_SCOPE_SYSTEM;
    attr->stack_size = 256u * 1024u;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t* attr) {
    (void)attr;
    return 0;
}

int pthread_attr_setscope(pthread_attr_t* attr, int scope) {
    if (attr) attr->scope = scope;
    return 0;
}

int pthread_attr_setschedpolicy(pthread_attr_t* attr, int policy) {
    if (attr) attr->policy = policy;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t* attr, size_t stacksize) {
    if (attr) attr->stack_size = stacksize;
    return 0;
}

int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                   void* (*start_routine)(void*), void* arg) {
    if (!thread || !start_routine) return EINVAL;
    size_t st = (attr && attr->stack_size >= 65536u) ? attr->stack_size : (256u * 1024u);
    int rc = pxe_thread_create_ex(thread, start_routine, arg, st, NULL);
    return (rc == 0) ? 0 : EAGAIN;
}

int pthread_join(pthread_t thread, void** retval) {
    int st = 0;
    int rc = pxe_thread_join(thread, &st);
    if (retval) *retval = (void*)(intptr_t)st;
    return (rc == 0) ? 0 : EINVAL;
}

pthread_t pthread_self(void) {
    return (pthread_t)(uint64_t)(unsigned int)pxe_getpid();
}

int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr) {
    (void)attr;
    if (!mutex) return EINVAL;
    pxe_mutex_init(mutex);
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* mutex) {
    (void)mutex;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t* mutex) {
    if (!mutex) return EINVAL;
    pxe_mutex_lock(mutex);
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t* mutex) {
    if (!mutex) return EINVAL;
    return pxe_mutex_trylock(mutex) == 0 ? 0 : EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t* mutex) {
    if (!mutex) return EINVAL;
    pxe_mutex_unlock(mutex);
    return 0;
}

int pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr) {
    (void)attr;
    if (!cond) return EINVAL;
    pxe_cond_init(cond);
    return 0;
}

int pthread_cond_destroy(pthread_cond_t* cond) {
    (void)cond;
    return 0;
}

int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    if (!cond || !mutex) return EINVAL;
    pxe_cond_wait(cond, mutex);
    return 0;
}

static uint64_t timespec_rel_ns(const struct timespec* abstime) {
    if (!abstime) return 0;
    uint64_t now_ns = pxe_get_time_ns();
    uint64_t tgt_ns = (uint64_t)abstime->tv_sec * 1000000000ULL + (uint64_t)abstime->tv_nsec;
    if (tgt_ns <= now_ns) return 1ULL;
    return tgt_ns - now_ns;
}

int pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex,
                           const struct timespec* abstime) {
    if (!cond || !mutex || !abstime) return EINVAL;
    uint64_t rel = timespec_rel_ns(abstime);
    int rc = pxe_cond_timedwait(cond, mutex, rel);
    return (rc == -110) ? ETIMEDOUT : 0;
}

int pthread_cond_signal(pthread_cond_t* cond) {
    if (!cond) return EINVAL;
    pxe_cond_signal(cond);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t* cond) {
    if (!cond) return EINVAL;
    pxe_cond_broadcast(cond);
    return 0;
}

int sem_init(sem_t* sem, int pshared, unsigned int value) {
    (void)pshared;
    if (!sem) { errno = EINVAL; return -1; }
    pxe_sem_init(sem, (uint32_t)value);
    return 0;
}

int sem_destroy(sem_t* sem) {
    (void)sem;
    return 0;
}

int sem_wait(sem_t* sem) {
    if (!sem) { errno = EINVAL; return -1; }
    pxe_sem_wait(sem);
    return 0;
}

int sem_trywait(sem_t* sem) {
    if (!sem) { errno = EINVAL; return -1; }
    if (pxe_sem_trywait(sem) == 0) return 0;
    errno = EAGAIN;
    return -1;
}

int sem_timedwait(sem_t* sem, const struct timespec* abs_timeout) {
    if (!sem || !abs_timeout) { errno = EINVAL; return -1; }
    for (;;) {
        if (pxe_sem_trywait(sem) == 0) return 0;
        uint64_t now_ns = pxe_get_time_ns();
        uint64_t tgt_ns = (uint64_t)abs_timeout->tv_sec * 1000000000ULL + (uint64_t)abs_timeout->tv_nsec;
        if (tgt_ns <= now_ns) {
            errno = ETIMEDOUT;
            return -1;
        }
        int rc = pxe_futex_wait(&sem->count, 0u, tgt_ns - now_ns);
        if (rc == -110) {
            errno = ETIMEDOUT;
            return -1;
        }
    }
}

int sem_post(sem_t* sem) {
    if (!sem) { errno = EINVAL; return -1; }
    pxe_sem_post(sem);
    return 0;
}

int sem_getvalue(sem_t* sem, int* sval) {
    if (!sem || !sval) { errno = EINVAL; return -1; }
    *sval = (int)__atomic_load_n(&sem->count, __ATOMIC_RELAXED);
    return 0;
}

/* =========================================================================
 * Freestanding Math Functions (SSE2 / polynomial)
 * ========================================================================= */

double fabs(double x) { return x < 0.0 ? -x : x; }
float  fabsf(float x) { return x < 0.0f ? -x : x; }

double floor(double x) {
    long long i = (long long)x;
    return (x < (double)i) ? (double)(i - 1) : (double)i;
}
float floorf(float x) { return (float)floor((double)x); }

double ceil(double x) {
    long long i = (long long)x;
    return (x > (double)i) ? (double)(i + 1) : (double)i;
}
float ceilf(float x) { return (float)ceil((double)x); }

long lrint(double x) {
    return (long)(x >= 0.0 ? (x + 0.5) : (x - 0.5));
}
long lrintf(float x) {
    return (long)(x >= 0.0f ? (x + 0.5f) : (x - 0.5f));
}

double sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double res;
    __asm__ volatile ("sqrtsd %1, %0" : "=x"(res) : "x"(x));
    return res;
}

float sqrtf(float x) {
    if (x <= 0.0f) return 0.0f;
    float res;
    __asm__ volatile ("sqrtss %1, %0" : "=x"(res) : "x"(x));
    return res;
}

double sin(double x) {
    const double two_pi = 6.28318530717958647692;
    const double pi = 3.14159265358979323846;
    x -= two_pi * floor((x + pi) / two_pi);
    double x2 = x * x;
    return x * (1.0 - x2 * (1.0 / 6.0 - x2 * (1.0 / 120.0 - x2 * (1.0 / 5040.0 - x2 * (1.0 / 362880.0 - x2 / 39916800.0)))));
}
float sinf(float x) { return (float)sin((double)x); }

double cos(double x) {
    return sin(x + 1.57079632679489661923);
}
float cosf(float x) { return (float)cos((double)x); }

double tan(double x) {
    double c = cos(x);
    if (fabs(c) < 1e-12) return 0.0;
    return sin(x) / c;
}

double atan(double x) {
    int neg = 0, inv = 0;
    if (x < 0.0) { neg = 1; x = -x; }
    if (x > 1.0) { inv = 1; x = 1.0 / x; }
    double x2 = x * x;
    double r = x * (1.0 - x2 * (1.0/3.0 - x2 * (1.0/5.0 - x2 * (1.0/7.0 - x2 * (1.0/9.0 - x2 * (1.0/11.0 - x2/13.0))))));
    if (inv) r = 1.57079632679489661923 - r;
    return neg ? -r : r;
}

double atan2(double y, double x) {
    if (x > 0.0) return atan(y / x);
    if (x < 0.0) return atan(y / x) + (y >= 0.0 ? M_PI : -M_PI);
    if (y > 0.0) return 1.57079632679489661923;
    if (y < 0.0) return -1.57079632679489661923;
    return 0.0;
}

double exp(double x) {
    if (x < -700.0) return 0.0;
    if (x > 700.0) x = 700.0;
    const double ln2 = 0.69314718055994530942;
    int k = (int)floor(x / ln2 + 0.5);
    double r = x - k * ln2;
    double term = 1.0, sum = 1.0;
    for (int i = 1; i <= 12; i++) {
        term *= r / (double)i;
        sum += term;
    }
    if (k >= 0) {
        while (k > 0) { sum *= 2.0; k--; }
    } else {
        while (k < 0) { sum *= 0.5; k++; }
    }
    return sum;
}

double log(double x) {
    if (x <= 0.0) return -1e300;
    int e = 0;
    while (x >= 2.0) { x *= 0.5; e++; }
    while (x < 1.0)  { x *= 2.0; e--; }
    double t = (x - 1.0) / (x + 1.0);
    double t2 = t * t;
    double p = t;
    double s = 0.0;
    for (int i = 1; i <= 15; i += 2) {
        s += p / (double)i;
        p *= t2;
    }
    return 2.0 * s + (double)e * 0.69314718055994530942;
}

double log10(double x) {
    return log(x) * 0.43429448190325182765;
}

double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x == 0.0) return 0.0;
    if (x < 0.0) {
        long long iy = (long long)y;
        double r = exp(y * log(-x));
        return (iy & 1LL) ? -r : r;
    }
    return exp(y * log(x));
}
