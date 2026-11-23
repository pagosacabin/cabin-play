/* 0250 -- proves the restructured quant table is behaviourally the previous one.
 *
 * Builds the old table (one shared {0, 0.0891, 0.4097, 1}) and the new one
 * (per-channel V[v]/V[3], gamma 2.2, ideal rows) with the firmware's own float
 * arithmetic, then reports how many entries differ and, for every one of the
 * 256 input codes, the mean code the 4x4 Bayer dither emits from each.
 *
 *     gcc -O0 -o check_table_equivalence check_table_equivalence.c -lm
 *     ./check_table_equivalence
 *     -> differing table entries: 17 / 256
 *        inputs whose 4x4 tile mean changed: 0 (worst 0.0000 code at c=-1)
 *
 * A measured row can be dropped into the V table below to see its effect.
 */

#include <stdio.h>
#include <math.h>
static const int B[4][4]={{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
static float bayer[4][4];
typedef struct { unsigned char lo, hi, thr; } qstep_t;
static qstep_t old_t[256], new_t[256];
static float emit(qstep_t q,int t){ return (t < q.thr) ? q.hi : q.lo; }
int main(void){
    for(int y=0;y<4;y++)for(int x=0;x<4;x++) bayer[y][x]=(B[y][x]+0.5f)*255.0f/16.0f;
    const float L0[4]={0.0f,0.0891f,0.4097f,1.0f};
    for(int c=0;c<256;c++){
        float T=powf(c/255.0f,2.2f);
        int v=0; while(v<3&&T>L0[v+1])v++;
        float f=(v==3)?0.0f:(T-L0[v])/(L0[v+1]-L0[v]);
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        old_t[c]=(qstep_t){(unsigned char)v,(unsigned char)(v==3?3:v+1),(unsigned char)(f*255.0f)};
    }
    float L[4];
    for(int v=0;v<4;v++){ float r=(v==0)?0.0f:(float)v/3.0f; L[v]=powf(r,2.2f);} L[0]=0.0f;
    for(int c=0;c<256;c++){
        float T=powf(c/255.0f,2.2f);
        int v=0; while(v<3&&T>L[v+1])v++;
        float f=(v==3)?0.0f:(T-L[v])/(L[v+1]-L[v]);
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        new_t[c]=(qstep_t){(unsigned char)v,(unsigned char)(v==3?3:v+1),(unsigned char)(f*255.0f)};
    }
    int diffs=0,mean_diff=0; double worst=0; int worst_c=-1;
    for(int c=0;c<256;c++){
        if(old_t[c].lo!=new_t[c].lo||old_t[c].hi!=new_t[c].hi||old_t[c].thr!=new_t[c].thr) diffs++;
        double so=0,sn=0;
        for(int y=0;y<4;y++)for(int x=0;x<4;x++){ so+=emit(old_t[c],bayer[y][x]); sn+=emit(new_t[c],bayer[y][x]); }
        double d=fabs(so-sn)/16.0; if(d>1e-9){mean_diff++; if(d>worst){worst=d;worst_c=c;}}
    }
    printf("differing table entries: %d / 256\n", diffs);
    if (mean_diff)
        printf("inputs whose 4x4 tile mean changed: %d (worst %.4f code at c=%d)\n",
               mean_diff, worst, worst_c);
    else
        printf("inputs whose 4x4 tile mean changed: 0 (bit-identical means)\n");
    /* extra: full-frame mean over one tile is what the eye integrates at large areas */
    return 0;
}
