#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <math.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sched.h>

#define die(...) fprintf(stderr,__VA_ARGS__), exit(1)

#define usage() die("Usage:\n\
\n\
    pifm [options] XXX.X < data\n\
\n\
Generate an FM signal with Raspberry Pi at specified frequency (in MHz),\n\
modulated with the single-channel 20000 samples/second 16-bit little-endian\n\
signed PCM stream presented on stdin.\n\
\n\
Options are:\n\
\n\
    -d Khz  - set the maximum FM deviation in Khz, default is 75\n\
    -t secs - exit after specified secs (or when input is exhausted, whichever comes first)\n\
\n\
The proper stream can be generated with sox, for example:\n\
\n\
    sox music.wav -c1 -r20000 -b16 -L -esigned - | sudo pifm 100.5\n\
\n\
Transmission ends when the source is exhausted or the process is killed.\n\
\n\
The FM signal is emitted from GPIO4, which is pin 7 on the 40-pin connector.\n\
")

#define BYTES_PER_SAMPLE 2
#define SAMPLES_PER_SECOND 20000 
#define USECS_PER_SAMPLE (1000000/SAMPLES_PER_SECOND)

// Defined by Makefile: older CPU is 0x20000000, newer CPU is 0x3f000000
#ifndef IOBASE
#error Must define IOBASE
#endif

// this is mmapped to IOBASE
void *iobase;

// Registers of interest
#define SYSCLK      *(volatile uint64_t *)(iobase+0x00003004)   // free-running 64-bit monotonic microsecond counter, initial value is undefined
#define GPCLK0_MODE *(volatile uint32_t *)(iobase+0x00101070)   // GPCLK0 control
#define GPCLK0_DIV  *(volatile uint32_t *)(iobase+0x00101074)   // GPCLK0 divisor
#define GPFSEL0     *(volatile uint32_t *)(iobase+0x00200000)   // Mux control for GPIO0 to GPIO9

volatile bool halt;                                             // set to force shutdown
pthread_t thread;                                               // transmit thread context

#define QMAX SAMPLES_PER_SECOND
uint32_t queue[QMAX];
uint32_t qhead=0, qcount=0;
pthread_mutex_t qmutex = PTHREAD_MUTEX_INITIALIZER;

// push data to queue and return true, or false if queue is full and data was not pushed
bool qpush(uint32_t d)
{
    bool res=false;
    pthread_mutex_lock(&qmutex);
    if (qcount < QMAX)
    {
        queue[(qhead+qcount)%QMAX]=d;
        qcount++;
        res=true;
    }
    pthread_mutex_unlock(&qmutex);
    return res;
}

// pull oldest data from queue and return true, or false if queue is empty
bool qpull(uint32_t *d)
{
    bool res=false;
    pthread_mutex_lock(&qmutex);
    if (qcount)
    {
        *d = queue[qhead];
        qcount--;
        qhead=(qhead+1)%QMAX;
        res = true;
    }
    pthread_mutex_unlock(&qmutex);
    return res;
}

// stop on various signals
void catch(int num)
{
    (void) num;
    halt=true;                                                  // force halt
}

// clean up on exit
void cleanup(void)
{
    halt=true;                                                  // force halt
    pthread_join(thread, NULL);                                 // and wait for thread (if not already)
}

// enable or disable GPCLK0 and GPIO4 output
inline void enable_GPCLK0(bool enable) 
{
    if (enable) 
    {
        GPFSEL0 = (GPFSEL0 & ~(7<<12)) | (4<<12);               // set GPIO4 as the GPCLK0 output
        GPCLK0_MODE = (0x5a<<24) | (1<<5);                      // set reset bit
        GPCLK0_MODE = (0x5a<<24) | (1<<4);                      // then enable
        GPCLK0_MODE = (0x5a<<24) | (1<<9) | (1<<4) | 6;         // then add MASH=1 and clock source=6 (PLLD == 500MHz)    
    } else
    {
        GPCLK0_MODE = 0x5a<<24;                                 // disable GPCLK0
        GPFSEL0 = GPFSEL0 & ~(7<<12);                           // set GPIO4 as an input
    }
}   

// When enabled, GPCLK0 source clock speed in Mhz
#define GPCLK0_SOURCE_MHZ 500.0

// Write GPCTL0 divisor 0 to 0xffffff, the GPIO4 frequency will be
// GPCLK0_SOURCE_MHZ*4096/divisor.
inline void set_GPCLK0(uint32_t n)
{
    GPCLK0_DIV = (0x5a<<24) | n;                   
}    
    
// This is a thread, write one queued sample to GPCLK0_DIV per
// USECS_PER_SAMPLE. Exit on queue underflow, or halt flag is set. Since it
// must update the GPCLK0 divisor in real time we give it a ridculously high
// priority and it basically hogs an entire CPU core. XXX use PCM and DMA to
// update the divisor register instead.  
void *transmit(void * unused)
{
    (void) unused;
  
    // set high priority
    pthread_setschedparam(thread, SCHED_FIFO, (struct sched_param []){{.sched_priority=90}});

    uint32_t divisor;
    while (!qpull(&divisor))                                    // wait for first queue entry
    {
        if (halt) return NULL;                                  // yeah, whatever
        pthread_yield();
    }

    enable_GPCLK0(1);                                           // enable GPCLK0 output
    uint64_t next = SYSCLK;                                     // next period starts now

    while (!halt)
    {
        while ((int64_t)(SYSCLK-next) < 0)                      // spin until end of period
        {
            if (halt) goto out;
        }
        set_GPCLK0(divisor);                                    // update GPCLK0_DIV
        if (!qpull(&divisor)) break;                            // get next divisor, or exit on underflow
        next += USECS_PER_SAMPLE;                               // set next period
        pthread_yield();
    }
    out:
    halt=true;                                                  // tell read thread, just in case
    enable_GPCLK0(0);                                                  
    return NULL;
}

int main(int argc, char* argv[])
{
    float frequency=0;                                          // Desired frequency, MHz
    float deviation=0.075;                                      // Desired deviation, Mhz (default 75Khz)
    time_t timeout=0;

    while (1) switch (getopt(argc,argv,":d:t:"))
    {
        case 'd': deviation=atof(optarg)/1000; break;           // specified in KHz
        case 't': timeout=strtoul(optarg,NULL,0); break;        
        case ':':                                               // missing
        case '?': usage();                                      // or invalid options
        case -1: goto optx;                                     // no more options
    } optx:
    argc-=optind-1; argv+=optind-1;
    if (argc == 2) frequency=atof(argv[1]);
    if (deviation<=0 || frequency<=0) usage();

    // mmap IOBASE
    int fd=open("/dev/mem", O_RDWR | O_SYNC);                   // mmap IOBASE
    if (fd < 0) die("Can't open /dev/mem: %s\n", strerror(errno));
    iobase = mmap(NULL, 0x002FFFFF, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IOBASE);
    if (iobase == MAP_FAILED) die("Can't mmap IOBASE %08X: %s\n", IOBASE, strerror(errno));
    close(fd);

    // start transmit thread
    int t = pthread_create(&thread, NULL, &transmit, NULL);
    if (t) die("Couldn't start transmitter thread: %s\n",strerror(t));

    // reap on exit
    atexit(cleanup);

    // catch various signals
    signal(SIGINT, catch);
    signal(SIGQUIT, catch);
    signal(SIGTERM, catch);
    signal(SIGHUP, catch);

    if (timeout) timeout += time(NULL);

    // read samples from stdin and queue for transmit
    while (!halt)
    {
        int16_t sample;
        int got=fread(&sample, BYTES_PER_SAMPLE, 1, stdin);     // fetch 16-bit sample from stdin
        if (!got) break;                                        // done if EOF (without setting halt flag)
        if (got != 1) die("stdin read failed: %s\n", strerror(errno));
        float f = frequency+((sample/32768.0)*deviation);       // convert to output frequency with deviation
        uint32_t d =(uint32_t)((GPCLK0_SOURCE_MHZ/f)*4096);     // convert to GPCLK0 divisor
        while (!qpush(d) && !halt) pthread_yield();             // push it to the transmit thread
        if (timeout && timeout <= time(NULL)) halt=true;        // halt on timeout 
    }
    pthread_join(thread, NULL);                                 // wait for transmit thread
    return 1;
}
