#define MY_MMAP_TRESHOLD 131072 //128KB
#define FREE 0 
#define IN_USE 1
#define ALIGN16(x) (((x) + 15) & (~15))

#define NTINY 32
#define TINY_END 512
#define TINY_ARENA_SIZE 65536 //64KB
#define TINY_STRIDE 16
#define TINY_BATCH_SIZE 32
#define TINY_CACHE_MAX 128

#define NSMALL 28
#define SMALL_END 4096  //4KB
#define SMALL_ARENA_SIZE 262144 //256KB
#define SMALL_STRIDE 128
#define SMALL_BATCH_SIZE 16
#define SMALL_CACHE_MAX 64

#define NMID 31
#define MID_END 131072 //128KB 
#define MID_ARENA_SIZE 2000000 //2MB#include <thread>
#define MID_STRIDE 4096 // 4KB
#define MID_BATCH_SIZE 4
#define MID_CACHE_MAX 16

#define MY_PROT_READ 1
#define MY_PROT_WRITE 2
#define MY_MAP_PRIVATE 2
#define MY_MAP_ANONYMOUS 0x20
#define MY_MAP_FAILED (void*)(-1)
#define MY_MREMAP_MAYMOVE 1

#ifndef NULL  
    #define NULL (void*) 0
#endif

typedef struct object_metadata {
    unsigned long size;
    short status;
    struct object_metadata* next;
    struct object_metadata* prev;    
} object_metadata;

typedef volatile int spinlock;

extern object_metadata* tiny_bins[NTINY];
extern object_metadata* small_bins[NSMALL];
extern object_metadata* mid_bins[NMID];

typedef struct {
    object_metadata* tiny_bins[NTINY];
    object_metadata* small_bins[NSMALL];
    object_metadata* mid_bins[NMID];

    int tiny_counts[NTINY];
    int small_counts[NSMALL];
    int mid_counts[NMID];
} thread_cache;
