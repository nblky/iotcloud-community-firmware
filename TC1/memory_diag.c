/* Temporary MK3031 diagnostic build. These layouts belong to kernel
 * 30300002.015 (SHA256 2f5dd55f8e702ff42c21d2606ec553731ce1cdefc2a90694d430559351e12874).
 * No allocation or Flash writes in the diagnostic reader. */
#include "mico.h"
#include "memory_diag.h"
#include <string.h>

#define WORD(a) (*(volatile const uint32_t *)(uintptr_t)(a))
#define ALLOCATED 0x80000000u
#define HEAP_ROWS 16u
#define TCP_ROWS 16u
static struct { uint32_t count, last_ms; } events[TC1_DIAG_EVENTS];
#define LIVE_ROWS 128u
typedef struct { uint32_t pointer, bytes, caller, since; } live_record;
static live_record live[LIVE_ROWS];
static uint32_t live_overflow, alloc_calls, free_calls;

extern uint32_t link_bss_end, _ram_end_;

static uint32_t irq_lock(void)
{
    uint32_t mask;
    __asm volatile("mrs %0, primask\ncpsid i" : "=r"(mask) :: "memory");
    return mask;
}
static void irq_unlock(uint32_t mask)
{ __asm volatile("msr primask, %0" :: "r"(mask) : "memory"); }
/* Tracks only application/newlib calls; the closed kernel bypasses these hooks. */
void tc1_diag_alloc(void *pointer,size_t bytes,uint32_t caller)
{
    if(!pointer)return;
    uint32_t now=mico_rtos_get_time(),m=irq_lock();alloc_calls++;
    for(unsigned i=0;i<LIVE_ROWS;i++)if(!live[i].pointer){
        live[i].pointer=(uint32_t)pointer;live[i].bytes=bytes;
        live[i].caller=caller;live[i].since=now;irq_unlock(m);return;
    }
    live_overflow++;irq_unlock(m);
}
void tc1_diag_free(void *pointer)
{
    if(!pointer)return;
    uint32_t m=irq_lock();free_calls++;
    for(unsigned i=0;i<LIVE_ROWS;i++)if(live[i].pointer==(uint32_t)pointer){live[i].pointer=0;break;}
    irq_unlock(m);
}
void *tc1_diag_realloc(void *pointer,size_t bytes,uint32_t caller,
                       void *(*resize)(void *,size_t))
{
    live_record saved={0};uint32_t m=irq_lock();
    for(unsigned i=0;i<LIVE_ROWS;i++)if(live[i].pointer==(uint32_t)pointer && pointer){
        saved=live[i];live[i].pointer=0;break;
    }
    irq_unlock(m);
    void *result=resize(pointer,bytes);
    if(result)tc1_diag_alloc(result,bytes,caller);
    else if(bytes && saved.pointer){
        m=irq_lock();unsigned i;
        for(i=0;i<LIVE_ROWS;i++)if(!live[i].pointer){live[i]=saved;break;}
        if(i==LIVE_ROWS)live_overflow++;
        irq_unlock(m);
    }
    return result;
}
static bool kernel_matches(void)
{
    return WORD(0x1f007248u)==0x41f0e92du &&
           WORD(0x1f006fe4u)==0x41f0e92du &&
           WORD(0x1f014da8u)==0xf7f2b538u;
}
static bool ram(uint32_t p,uint32_t bytes)
{
    if(p&3u)return false;
    return (p>=0x00100000u && p<0x00160000u && bytes<=0x00160000u-p) ||
           (p>=0x20000000u && p<0x20018000u && bytes<=0x20018000u-p);
}
void tc1_diag_event(unsigned event)
{
    uint32_t now=mico_rtos_get_time(),mask=irq_lock();
    if(event<TC1_DIAG_EVENTS){events[event].count++;events[event].last_ms=now;}
    irq_unlock(mask);
}
static void numbers(ph_writer *w,const char *key,const uint32_t *v,unsigned n)
{
    phw_begin(w,key,true);
    for(unsigned i=0;i<n;i++)phw_u32(w,NULL,v[i]);
    phw_end(w);
}

void tc1_heap_diag_mp(ph_writer *w,uint32_t after)
{
    /* Snapshot headers while scheduling is disabled; encode after restoring IRQs.
     * Regions are the allocator's actual descriptors, not guessed heap bounds. */
    uint32_t rows[HEAP_ROWS][10],regions[6]={0},hist[12]={0};
    uint32_t n=0,blocks=0,free_sum=0,used_sum=0,largest=0,next=0;
    uint32_t total=0,free_bytes=0,minimum=0,bad=0;
    bool supported=kernel_matches(),ok=supported;
    uint32_t mask=irq_lock();
    if(supported){
        total=WORD(0x00103990u);free_bytes=WORD(0x00104848u);
        minimum=WORD(0x00104858u);
        /* moc_app_main additionally inserts the unused application RAM. Its
         * descriptors are not appended to the kernel's original region table. */
        regions[0]=WORD(0x00104830u);regions[1]=WORD(0x00104834u);
        regions[2]=(uint32_t)&link_bss_end;
        regions[3]=(uint32_t)&_ram_end_-regions[2];
        regions[4]=WORD(0x00104838u);regions[5]=WORD(0x0010483cu);
        if(WORD(0x0010485cu)!=ALLOCATED)ok=false;
        for(unsigned r=0;ok && r<3 && regions[r*2+1];r++){
            uint32_t base=regions[r*2],bytes=regions[r*2+1];
            if(bytes<16 || !ram(base&~3u,bytes+(base&3u))){ok=false;bad=base;break;}
            uint32_t p=(base+7u)&~7u,end=(base+bytes-8u)&~7u;
            while(p<end){
                uint32_t raw=WORD(p+4),size=raw&~ALLOCATED;
                if(++blocks>4096 || size<8 || (size&7u) || size>end-p){ok=false;bad=p;break;}
                if(raw&ALLOCATED){
                    used_sum+=size;
                    unsigned bucket=0;uint32_t bound=16;
                    while(size>bound && bucket<11){bucket++;bound*=2;}
                    hist[bucket]++;
                    if(p+8>after){
                        if(n<HEAP_ROWS){
                            uint32_t *row=rows[n++],hash=2166136261u;
                            row[0]=p+8;row[1]=size;
                            unsigned sample=size-8<64?size-8:64;
                            for(unsigned j=0;j<sample;j++)hash=(hash^*(volatile const unsigned char *)(uintptr_t)(p+8+j))*16777619u;
                            row[2]=hash;
                            row[7]=row[8]=row[9]=0;
                            for(unsigned k=0;k<LIVE_ROWS;k++)if(live[k].pointer==row[0]){
                                row[7]=live[k].caller;row[8]=mico_rtos_get_time()-live[k].since;
                                row[9]=live[k].bytes;break;
                            }
                            /* Pointer shape helps identify owners without exporting strings/secrets. */
                            for(unsigned j=0;j<4;j++){
                                uint32_t value=size>=12+4*j?WORD(p+8+4*j):0;
                                row[3+j]=ram(value,4)?value:0;
                            }
                        }else if(!next)next=rows[n-1][0];
                    }
                }else{
                    free_sum+=size;if(size>largest)largest=size;
                }
                p+=size;
            }
            if(ok && r!=1 && WORD(end+4)!=0){ok=false;bad=end;}
        }
        if(free_sum!=free_bytes || free_sum+used_sum!=total)ok=false;
    }
    irq_unlock(mask);
    phw_begin(w,NULL,false);phw_text(w,"code","ok");phw_begin(w,"data",false);
    phw_bool(w,"supported",supported);phw_bool(w,"consistent",ok);
    phw_u32(w,"ms",mico_rtos_get_time());phw_u32(w,"total",total);
    phw_u32(w,"free",free_bytes);phw_u32(w,"minimum",minimum);
    phw_u32(w,"walk_free",free_sum);phw_u32(w,"walk_used",used_sum);
    phw_u32(w,"largest",largest);phw_u32(w,"blocks",blocks);
    phw_u32(w,"bad",bad);phw_u32(w,"next",next);
    phw_u32(w,"tracker_overflow",live_overflow);
    phw_u32(w,"alloc_calls",alloc_calls);phw_u32(w,"free_calls",free_calls);
    phw_u32(w,"tracker_bytes",sizeof(live));
    numbers(w,"regions",regions,6);numbers(w,"histogram",hist,12);
    phw_begin(w,"rows",true);
    for(unsigned i=0;i<n;i++)numbers(w,NULL,rows[i],10);
    phw_end(w);phw_end(w);phw_end(w);
}

static uint32_t allocation_size(uint32_t p,uint32_t minimum)
{
    if(p<8 || !ram(p-8,minimum+8))return 0;
    uint32_t raw=WORD(p-4),size=raw&~ALLOCATED;
    return (raw&ALLOCATED) && !(size&7u) && size>=minimum+8 && ram(p-8,size)?size:0;
}
void tc1_net_diag_mp(ph_writer *w)
{
    uint32_t rows[TCP_ROWS][13],n=0,counts[2]={0},bad=0;
    uint32_t event_copy[TC1_DIAG_EVENTS*2];
    bool supported=kernel_matches(),ok=supported;
    uint32_t mask=irq_lock();
    memcpy(event_copy,events,sizeof(event_copy));
    if(supported)for(unsigned list=0;ok && list<2;list++){
        uint32_t p=WORD(list?0x00105248u:0x00105240u);
        while(p){
            uint32_t size=allocation_size(p,158);
            if(!size || ++counts[list]>64){ok=false;bad=p;break;}
            if(n<TCP_ROWS){
                uint32_t *row=rows[n++],conn=WORD(p+16);
                row[0]=list;row[1]=p;row[2]=size;
                row[3]=*(volatile const unsigned char *)(uintptr_t)(p+24);
                row[8]=*(volatile const unsigned char *)(uintptr_t)(p+70);
                row[9]=*(volatile const uint16_t *)(uintptr_t)(p+52);
                row[10]=*(volatile const uint16_t *)(uintptr_t)(p+68);
                row[11]=*(volatile const unsigned char *)(uintptr_t)(p+156);
                row[12]=*(volatile const unsigned char *)(uintptr_t)(p+157);
                row[4]=conn;row[5]=0;row[6]=WORD(p);row[7]=WORD(p+4);
                if(allocation_size(conn,52) && WORD(conn+4)==p)
                    row[5]=*(volatile const unsigned char *)(uintptr_t)(conn+1);
            }
            p=WORD(p+12);
        }
    }
    irq_unlock(mask);
    phw_begin(w,NULL,false);phw_text(w,"code","ok");phw_begin(w,"data",false);
    phw_bool(w,"supported",supported);phw_bool(w,"consistent",ok);
    phw_u32(w,"ms",mico_rtos_get_time());phw_u32(w,"bad",bad);
    numbers(w,"counts",counts,2);numbers(w,"events",event_copy,TC1_DIAG_EVENTS*2);
    phw_begin(w,"rows",true);
    for(unsigned i=0;i<n;i++)numbers(w,NULL,rows[i],13);
    phw_end(w);phw_end(w);phw_end(w);
}
