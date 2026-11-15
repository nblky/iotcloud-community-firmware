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
#define RX_ROWS 2u
#define RX_WORDS 88u
#define BYTE(a) (*(volatile const unsigned char *)(uintptr_t)(a))
#define HALF(a) (*(volatile const uint16_t *)(uintptr_t)(a))
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

static uint32_t allocation_size(uint32_t p,uint32_t minimum);

/* Only known kernel object layouts are exported, never packet contents.
 * Called with IRQs disabled, so slot ownership and timer fields agree. */
static unsigned rx_object(uint32_t p,uint32_t size,uint32_t *row)
{
    row[1]=p;row[2]=size;
    if(size==1608 && WORD(p+4)==p+16 && BYTE(p+12)==3){
        row[0]=1;row[3]=WORD(p);row[4]=WORD(p+4);
        row[5]=HALF(p+8);row[6]=HALF(p+10);
        row[7]=BYTE(p+12);row[8]=BYTE(p+13);row[9]=HALF(p+14);
        return 10;
    }
    if(size>=80 && size<=96 && BYTE(p+40)==2){
        uint32_t buffer=WORD(p+24),rxpd=WORD(p+28);
        if(allocation_size(buffer,1596)==1608 &&
           WORD(buffer+4)==buffer+16 && BYTE(buffer+12)==3 &&
           WORD(p+20)==buffer+16 && allocation_size(rxpd,20)){
            row[0]=2;row[3]=WORD(p+16);row[4]=WORD(p+20);
            row[5]=buffer;row[6]=rxpd;row[7]=WORD(p+32);
            row[8]=WORD(p+36);row[9]=BYTE(p+40);
            row[10]=HALF(buffer+14);row[11]=allocation_size(rxpd,20);
            return 12;
        }
    }
    /* Reorder entries request 60 bytes; small unsplit heap tails can add 16. */
    if(size>=72 && size<=88 && WORD(p+44)==p && WORD(p+8)<16){
        uint32_t count=WORD(p+28),array=WORD(p+32),timer=WORD(p+36);
        if(!count || count>64 || !allocation_size(array,count*4))return 0;
        row[0]=3;row[3]=WORD(p+8);row[4]=WORD(p+20);
        row[5]=WORD(p+24);row[6]=count;row[7]=array;row[8]=timer;
        row[9]=BYTE(p+40);row[10]=WORD(p+48);
        row[11]=row[12]=row[13]=row[14]=row[15]=row[16]=row[17]=0;
        /* Actual xTimerIsTimerActive reads container at +20; ID is +32.
         * Do not call RTOS timer functions while holding the IRQ snapshot. */
        if(allocation_size(timer,40) && WORD(timer+32)==p+36 &&
           (WORD(timer+36)&~1u)==0x1f040164u){
            row[11]=1;row[12]=WORD(timer+20)!=0;
            row[13]=WORD(timer+24);row[14]=WORD(timer+4);
            row[15]=WORD(timer+32);row[16]=WORD(timer+36);
            row[17]=WORD(timer+20);
        }
        row[18]=WORD(p);row[19]=WORD(p+4);row[20]=0;
        for(unsigned i=0;i<count;i++){
            row[21+i]=WORD(array+4*i);
            if(row[21+i])row[20]++;
        }
        return 21+count;
    }
    return 0;
}

static void heap_diag_mp(ph_writer *w,uint32_t after,bool rx)
{
    /* Snapshot headers while scheduling is disabled; encode after restoring IRQs.
     * Regions are the allocator's actual descriptors, not guessed heap bounds. */
    uint32_t rows[HEAP_ROWS][10],rx_rows[RX_ROWS][RX_WORDS],rx_lengths[RX_ROWS];
    uint32_t regions[6]={0},hist[12]={0},rx_count=0;
    uint32_t n=0,blocks=0,free_sum=0,used_sum=0,largest=0,next=0;
    uint32_t total=0,free_bytes=0,minimum=0,bad=0;
    bool supported=kernel_matches() && (!rx ||
        (WORD(0x1f03ae2cu)==0x4606b5f8u &&
         WORD(0x1f0403f0u)==0x4ff8e92du &&
         WORD(0x1f006f78u)==0x4604b510u)),ok=supported;
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
                    if(rx){
                        uint32_t candidate[RX_WORDS];
                        unsigned length=rx_object(p+8,size,candidate);
                        if(length){
                            rx_count++;
                            if(p+8>after){
                                if(n<RX_ROWS){
                                    memcpy(rx_rows[n],candidate,length*sizeof(uint32_t));
                                    rx_lengths[n++]=length;
                                }else if(!next)next=rx_rows[n-1][1];
                            }
                        }
                    }else if(p+8>after){
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
    if(rx){phw_u32(w,"schema",1);phw_u32(w,"rx_objects",rx_count);}
    numbers(w,"regions",regions,6);numbers(w,"histogram",hist,12);
    phw_begin(w,"rows",true);
    for(unsigned i=0;i<n;i++)
        if(rx)numbers(w,NULL,rx_rows[i],rx_lengths[i]);
        else numbers(w,NULL,rows[i],10);
    phw_end(w);phw_end(w);phw_end(w);
}

void tc1_heap_diag_mp(ph_writer *w,uint32_t after)
{ heap_diag_mp(w,after,false); }
void tc1_rx_diag_mp(ph_writer *w,uint32_t after)
{ heap_diag_mp(w,after,true); }

/* Read-only, per-chunk atomic RAM capture for offline ownership analysis.
 * Separate chunks are not a globally frozen snapshot. Keep copies on the
 * caller's stack rather than reserving another permanent diagnostic buffer. */
void tc1_ram_diag_mp(ph_writer *w,uint32_t address,uint32_t length)
{
    unsigned char bytes[512];
    uint32_t ms=0,free_bytes=0;
    bool supported=kernel_matches(),valid=false;
    uint32_t mask=irq_lock();
    if(supported && length && length<=sizeof(bytes)){
        uint32_t base=WORD(0x00104838u),size=WORD(0x0010483cu);
        valid=(address>=0x00100000u && address<(uint32_t)&_ram_end_ &&
               length<=(uint32_t)&_ram_end_-address) ||
              (ram(base&~3u,size+(base&3u)) && address>=base &&
               address-base<size && length<=size-(address-base));
        if(valid){
            for(unsigned i=0;i<length;i++)bytes[i]=BYTE(address+i);
            ms=mico_rtos_get_time();free_bytes=WORD(0x00104848u);
        }
    }
    irq_unlock(mask);
    phw_begin(w,NULL,false);
    phw_text(w,"code",valid?"ok":"invalid_request");
    phw_begin(w,"data",false);phw_bool(w,"supported",supported);
    if(valid){
        phw_u32(w,"address",address);phw_u32(w,"ms",ms);
        phw_u32(w,"free",free_bytes);phw_bytes_value(w,"bytes",bytes,length);
    }
    phw_end(w);phw_end(w);
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
