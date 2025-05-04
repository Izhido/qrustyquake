// Copyright (C) 1996-2001 Id Software, Inc.
// Copyright (C) 2002-2009 John Fitzgibbons and others
// Copyright (C) 2007-2008 Kristian Duske
// GPLv3 See LICENSE for details.

#define M_PI_DIV_180 (M_PI / 180.0) //johnfitz
#define Q_rint(x) ((x) > 0 ? (int)((x) + 0.5) : (int)((x) - 0.5)) // johnfitz -- from joequake
#define DotProduct(x,y) (x[0]*y[0]+x[1]*y[1]+x[2]*y[2])
#define DoublePrecisionDotProduct(x,y) ((double)(x)[0]*(y)[0]+(double)(x)[1]*(y)[1]+(double)(x)[2]*(y)[2])
#define VectorSubtract(a,b,c) {c[0]=a[0]-b[0];c[1]=a[1]-b[1];c[2]=a[2]-b[2];}
#define VectorAdd(a,b,c) {c[0]=a[0]+b[0];c[1]=a[1]+b[1];c[2]=a[2]+b[2];}
#define VectorCopy(a,b) {b[0]=a[0];b[1]=a[1];b[2]=a[2];}
#define LERP(a, b, t) ((a) + ((b)-(a))*(t))
#define IS_NAN(x) isnan(x)
// johnfitz -- courtesy of lordhavoc
#define VectorNormalizeFast(_v)                                           \
{                                                                         \
        float _y, _number;                                                \
        _number = DotProduct(_v, _v);                                     \
        if (_number != 0.0)                                               \
        {                                                                 \
                *((int *)&_y) = 0x5f3759df - ((* (int *) &_number) >> 1); \
                _y = _y * (1.5f - (_number * 0.5f * _y * _y));            \
                VectorScale(_v, _y, _v);                                  \
        }                                                                 \
}
#define BOX_ON_PLANE_SIDE(emins, emaxs, p)                          \
        (((p)->type < 3) ? (                                        \
                ((p)->dist <= (emins)[(p)->type]) ? 1               \
                      : (((p)->dist >= (emaxs)[(p)->type]) ? 2 : 3) \
        ) : BoxOnPlaneSide((emins), (emaxs), (p)))

typedef float vec_t;
typedef vec_t vec3_t[3];
typedef vec_t vec5_t[5];
typedef int fixed4_t;
typedef int fixed8_t;
typedef int fixed16_t;

struct mplane_s;
extern vec3_t vec3_origin;
extern int nanmask;

void VectorMA(vec3_t veca, float scale, vec3_t vecb, vec3_t vecc);
vec_t Length(vec3_t v);
void CrossProduct(vec3_t v1, vec3_t v2, vec3_t cross);
float VectorNormalize(vec3_t v); // returns vector length
void VectorInverse(vec3_t v);
void VectorScale(vec3_t in, vec_t scale, vec3_t out);
void R_ConcatRotations(float in1[3][3], float in2[3][3], float out[3][3]);
void R_ConcatTransforms(float in1[3][4], float in2[3][4], float out[3][4]);
void FloorDivMod(double numer, double denom, int *quotient, int *rem);
int GreatestCommonDivisor(int i1, int i2);
void AngleVectors(vec3_t angles, vec3_t forward, vec3_t right, vec3_t up);
int BoxOnPlaneSide(vec3_t emins, vec3_t emaxs, struct mplane_s *plane);
float anglemod(float a);
vec_t VectorLength(vec3_t v);
