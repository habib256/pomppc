/* GPL3 - Copyleft VERHILLE Arnaud */
/*
 * iotrace.c — trace des appels de l'IOProc CoreAudio d'une application Tiger
 * (saccades du son de DOOM 3, 29/09/2026).
 *
 * Bibliothèque à insérer (DYLD_INSERT_LIBRARIES) : les fonctions
 * AudioDeviceAddIOProc / Start / Stop / RemoveIOProc sont interposées
 * (section __DATA,__interpose, dyld de Tiger), l'IOProc de l'application est
 * enveloppé. À chaque appel on range, sans E/S dans le fil temps réel :
 *   entrée et sortie (mach_absolute_time), inNow et inOutputTime (temps
 *   d'échantillon et temps hôte), taille du tampon de sortie.
 * Un fil ordinaire vide l'anneau dans $IOTRACE (CSV) toutes les 500 ms.
 *
 * Colonnes (µs depuis le premier appel, temps d'échantillon en trames) :
 *   n,entree_us,duree_us,now_st,out_st,marge_us,saut_st,octets
 * marge_us = inOutputTime.mHostTime - sortie : le temps qui restait avant que
 * la première trame produite soit jouée (négatif = en retard).
 * saut_st = out_st - (out_st précédent + trames du tampon précédent) : 0 en
 * régime normal ; autre chose = le HAL a sauté ou repris des trames.
 *
 *   gcc-4.0 -dynamiclib -o iotrace.dylib iotrace.c -framework CoreAudio
 */
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct {
    uint64_t t0, t1, now_ht, out_ht;
    double now_st, out_st;
    uint32_t bytes;
} Rec;

#define NREC 65536
static Rec ring[NREC];
static volatile uint32_t wpos, rpos;

static AudioDeviceIOProc real_proc;
static FILE *out;
static mach_timebase_info_data_t tb;
static uint64_t base;
static double prev_st = -1, prev_frames;
static uint32_t nrec;

static double us(uint64_t t)
{
    return (double)(t - base) * tb.numer / tb.denom / 1000.0;
}

static void *writer(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(500000);
        while (rpos != wpos) {
            Rec *r = &ring[rpos % NREC];
            double marge, saut = 0;
            double frames = r->bytes / 8.0;     /* float32 stéréo */

            if (!base) {
                base = r->t0;
            }
            marge = ((double)(int64_t)(r->out_ht - r->t1)) * tb.numer / tb.denom / 1000.0;
            if (prev_st >= 0) {
                saut = r->out_st - (prev_st + prev_frames);
            }
            prev_st = r->out_st;
            prev_frames = frames;
            fprintf(out, "%u,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%u\n", nrec++, us(r->t0),
                    (double)(r->t1 - r->t0) * tb.numer / tb.denom / 1000.0,
                    r->now_st, r->out_st, marge, saut, r->bytes);
            rpos++;
        }
        fflush(out);
    }
    return NULL;
}

static OSStatus wrap_proc(AudioDeviceID dev, const AudioTimeStamp *now,
                          const AudioBufferList *in, const AudioTimeStamp *inTime,
                          AudioBufferList *outd, const AudioTimeStamp *outTime,
                          void *cd)
{
    uint64_t t0 = mach_absolute_time();
    OSStatus r = real_proc(dev, now, in, inTime, outd, outTime, cd);
    uint64_t t1 = mach_absolute_time();

    if (wpos - rpos < NREC) {
        Rec *x = &ring[wpos % NREC];

        x->t0 = t0;
        x->t1 = t1;
        x->now_ht = now ? now->mHostTime : 0;
        x->now_st = now ? now->mSampleTime : 0;
        x->out_ht = outTime ? outTime->mHostTime : 0;
        x->out_st = outTime ? outTime->mSampleTime : 0;
        x->bytes = (outd && outd->mNumberBuffers) ? outd->mBuffers[0].mDataByteSize : 0;
        wpos++;
    }
    return r;
}

static void start_writer(void)
{
    static int started;
    pthread_t th;
    const char *p;
    struct timeval tv;

    if (started) {
        return;
    }
    started = 1;
    mach_timebase_info(&tb);
    p = getenv("IOTRACE");
    out = fopen(p && *p ? p : "/tmp/iotrace.csv", "w");
    if (!out) {
        return;
    }
    gettimeofday(&tv, NULL);
    fprintf(out, "# debut %ld.%06ld mach %llu\n", (long)tv.tv_sec, (long)tv.tv_usec,
            (unsigned long long)mach_absolute_time());
    fprintf(out, "n,entree_us,duree_us,now_st,out_st,marge_us,saut_st,octets\n");
    fflush(out);
    pthread_create(&th, NULL, writer, NULL);
}

static OSStatus my_add(AudioDeviceID dev, AudioDeviceIOProc proc, void *data)
{
    start_writer();
    real_proc = proc;
    return AudioDeviceAddIOProc(dev, wrap_proc, data);
}

static AudioDeviceIOProc map(AudioDeviceIOProc proc)
{
    return (proc && proc == real_proc) ? wrap_proc : proc;
}

static OSStatus my_start(AudioDeviceID dev, AudioDeviceIOProc proc)
{
    return AudioDeviceStart(dev, map(proc));
}

static OSStatus my_stop(AudioDeviceID dev, AudioDeviceIOProc proc)
{
    return AudioDeviceStop(dev, map(proc));
}

static OSStatus my_remove(AudioDeviceID dev, AudioDeviceIOProc proc)
{
    return AudioDeviceRemoveIOProc(dev, map(proc));
}

__attribute__((used)) static const struct { const void *nouveau, *ancien; }
interposers[] __attribute__((section("__DATA,__interpose"))) = {
    { (const void *)my_add, (const void *)AudioDeviceAddIOProc },
    { (const void *)my_start, (const void *)AudioDeviceStart },
    { (const void *)my_stop, (const void *)AudioDeviceStop },
    { (const void *)my_remove, (const void *)AudioDeviceRemoveIOProc },
};
