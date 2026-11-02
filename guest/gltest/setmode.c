/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * setmode.c — change le mode de l'écran principal de Tiger pour la session,
 * sans passer par Préférences Système (balayage des résolutions sous Quartz
 * Extreme, docs/quartz-extreme.md, journal du 02/11).
 *
 *   setmode                  liste les modes
 *   setmode largeur hauteur profondeur
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
static long num(CFDictionaryRef d, CFStringRef k){ long v=0; CFNumberRef n=CFDictionaryGetValue(d,k); if(n) CFNumberGetValue(n,kCFNumberLongType,&v); return v; }
int main(int c,char**a){
  CGDirectDisplayID d=CGMainDisplayID();
  if(c<4){ CFArrayRef m=CGDisplayAvailableModes(d); long i; for(i=0;i<CFArrayGetCount(m);i++){ CFDictionaryRef e=CFArrayGetValueAtIndex(m,i); printf("%ldx%ldx%ld\n",num(e,kCGDisplayWidth),num(e,kCGDisplayHeight),num(e,kCGDisplayBitsPerPixel)); } return 0; }
  boolean_t ex=0; CFDictionaryRef m=CGDisplayBestModeForParameters(d,atoi(a[3]),atoi(a[1]),atoi(a[2]),&ex);
  CGDisplayConfigRef cfg; CGBeginDisplayConfiguration(&cfg); CGConfigureDisplayMode(cfg,d,m);
  CGDisplayErr r=CGCompleteDisplayConfiguration(cfg,kCGConfigureForSession);
  printf("demande %sx%sx%s exact=%d rc=%d ecran=%ldx%ldx%ld pas=%ld\n",a[1],a[2],a[3],ex,r,(long)CGDisplayPixelsWide(d),(long)CGDisplayPixelsHigh(d),(long)CGDisplayBitsPerPixel(d),(long)CGDisplayBytesPerRow(d));
  return r!=0;
}
