#ifndef _VPLAYER_COMPAT_MATH_H
#define _VPLAYER_COMPAT_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

double fabs(double x);
float  fabsf(float x);
double floor(double x);
float  floorf(float x);
double ceil(double x);
float  ceilf(float x);
double sqrt(double x);
float  sqrtf(float x);
double sin(double x);
float  sinf(float x);
double cos(double x);
float  cosf(float x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double exp(double x);
double log(double x);
double log10(double x);
double pow(double x, double y);
long   lrint(double x);
long   lrintf(float x);

#ifdef __cplusplus
}
#endif

#endif

