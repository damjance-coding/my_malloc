#include "heap_internal.h"

void* my_mmap(void *addr, unsigned long len, int prot, int flags, int fd, long offest) {
    void *ptr;
    register int r10_val __asm__("r10") = flags;
    register int r8_val  __asm__("r8") = fd;
    register int r9_val  __asm__("r9") = offest;
    __asm__ __volatile__ (
        "syscall\n\t"
        :"=a"(ptr)
        :"a" (9), "D"(addr), "S"(len), "d"(prot), "r"(r10_val), "r"(r8_val), "r"(r9_val)
        : "rcx", "r11", "memory"
    );
    return  ptr;
}

int my_munmap(void* addr, long len) {
    int ret;
    __asm__ __volatile__ (
        "syscall\n\t"
        :"=a"(ret)
        :"a"(11), "D"(addr), "S"(len)
        : "rcx", "r11", "memory"
    );
    return ret;
}

void* my_mremap(void* addr, unsigned long old_len, unsigned long new_leng, int flags ) {
    void* ptr;

    register int r10_val __asm__("r10") = flags;
    __asm__ __volatile__(
        "syscall\n\t"
        : "=a" (ptr)
        : "a"(25),"D"(addr), "S"(old_len), "d"(new_leng), "r"(r10_val)
        : "rcx", "r11", "memory"
    ); 
    return ptr;
}

static inline int atomic_xchg(spinlock *lock, int val) {
    int prev;
    __asm__ __volatile__ (
        "lock xchg %0, %1"
        : "=r" (prev), "+m" (*lock)
        : "0" (val)
        : "memory"
    );
    return prev;
}

void spin_acquire(spinlock *lock) {
    while (atomic_xchg(lock, 1)) {
        __asm__ __volatile__ ("pause");
    }
}

void spin_release(spinlock *lock) {
    *lock = 0;
}

spinlock tiny_locks[NTINY]   = {0};
spinlock small_locks[NSMALL] = {0};
spinlock mid_locks[NMID]     = {0};


static __thread thread_cache* tcache = &((thread_cache){0});

object_metadata* carve_arena(void* arena, unsigned long arena_size, unsigned long obj_size) {
    long metadata_stride =  sizeof(object_metadata) + obj_size;
    object_metadata* head = NULL;
    for(unsigned long i = 0; i  < arena_size - metadata_stride; i += metadata_stride) {
        object_metadata* obj = (object_metadata*)((char*)(arena) +i);
        
        obj->size = obj_size; 

        if(!head) {
            head = obj;
            obj->next = (object_metadata*)((char*)(arena) + i + metadata_stride);
        }
        else {
            if(i + metadata_stride >= arena_size - metadata_stride) {
                obj->next = NULL;
            }else {
                obj->next = (object_metadata*)((char*)(arena) + i + metadata_stride);
            }
        }
        

    }
    return head;
};

object_metadata* get_object_ptr(void* ptr) {
    return (object_metadata*)ptr - 1;
}

object_metadata* tiny_bins[NTINY] = {0};
object_metadata* small_bins[NSMALL] = {0};
object_metadata* mid_bins[NMID] = {0};

void* my_malloc(unsigned long size) {
    // 1. Sanity check
    if(size == 0) {
        return NULL;
    }
    // 2. Align size to 16 bytes
    size = ALIGN16(size);

    // 3. Check if the memory object needs to be allocated via mmap dirrectly
    if(size > MY_MMAP_TRESHOLD) {
        object_metadata* lob = my_mmap(NULL, sizeof(object_metadata) + size, MY_PROT_READ | MY_PROT_WRITE, MY_MAP_PRIVATE | MY_MAP_ANONYMOUS, -1, 0);
        // LOB stands for Large OBject
        
        if(lob == MY_MAP_FAILED) {
            return NULL;
        }

        lob->size = size;
        lob->next = IN_USE;

        return (void*)(lob + 1);
    };

    // 4. Handle allocations 16-512 bytes
    if(size <= TINY_END) {
        short idx = size / TINY_STRIDE - 1; // The increment between objects in the tiny_bin is 16b
        // Check the thread cache first (no need for a spinlock)
        if(tcache->tiny_bins[idx]) {
            object_metadata* sob = tcache->tiny_bins[idx];

            tcache->tiny_bins[idx] = sob->next;
            sob->next = IN_USE; // next now representes the status, if it isnt IN_USE the object is free

            tcache->tiny_counts[idx]--;

            return (void*)(sob+1);
        }

        ///thread cache empty, take from tiny_bins[idx]
        spin_acquire(&tiny_locks[idx]);
        if(tiny_bins[idx]) {
            for(int i = 0; i < TINY_BATCH_SIZE; i++) {
                object_metadata* sob = tiny_bins[idx];
                if(sob == NULL) break;

                tiny_bins[idx] = sob->next; 
                sob->next = tcache->tiny_bins[idx];

                tcache->tiny_bins[idx] = sob;                
            }

            object_metadata* head = tcache->tiny_bins[idx];

            tcache->tiny_bins[idx] = head->next;
            head->next = IN_USE;

            tcache->tiny_counts[idx] = TINY_BATCH_SIZE -1;

            spin_release(&tiny_locks[idx]);
            return (void*)(head + 1);
        }

        spin_release(&tiny_locks[idx]);
        void* arena= my_mmap(NULL, TINY_ARENA_SIZE, MY_PROT_READ | MY_PROT_WRITE, MY_MAP_PRIVATE | MY_MAP_ANONYMOUS, -1, 0);
        
        if(arena == MY_MAP_FAILED) {
            return NULL;
        }

        object_metadata* head = carve_arena(arena, TINY_ARENA_SIZE, size);
        spin_acquire(&tiny_locks[idx]);
        tiny_bins[idx] = head;

        for(int i = 0; i < TINY_BATCH_SIZE; i++) {
            object_metadata* sob = tiny_bins[idx];

            if(sob == NULL) break;

            tiny_bins[idx] = sob->next;

            sob->next = tcache->tiny_bins[idx];
            tcache->tiny_bins[idx] = sob;
        }

        tcache->tiny_counts[idx] = TINY_BATCH_SIZE;

        head->next = IN_USE;

        spin_release(&tiny_locks[idx]);
        return (void*)(head + 1);
    }

    // 5. Handle allocations 528-4KB
    else if (size <= SMALL_END) {

        short idx = size / SMALL_STRIDE - 1; // The increment between objects in the small_bin is 128b
        
        if(tcache->small_bins[idx]) {
            object_metadata* sob = tcache->small_bins[idx];

            tcache->small_bins[idx] = sob->next;
            sob->next = IN_USE;

            tcache->small_counts[idx]--;

            return (void*)(sob + 1);
        }
        else {
            spin_acquire(&small_locks[idx]);
            if(small_bins[idx]) {
                for(int i = 0; i < SMALL_BATCH_SIZE; i++) {
                    object_metadata* sob = small_bins[idx];
                    if(sob == NULL) break;

                    small_bins[idx] = sob-> next;

                    sob->next = tcache->small_bins[idx];
                    tcache->small_bins[idx] = sob;
                }

                object_metadata* head = tcache->small_bins[idx];
                
                tcache->small_bins[idx] = head->next;
                head->next = IN_USE;

                tcache->small_counts[idx] = SMALL_BATCH_SIZE -1;

                spin_release(&small_locks[idx]);
                return (void*)(head + 1);
            
            }   

            spin_release(&small_locks[idx]);
            void* arena= my_mmap(NULL, SMALL_ARENA_SIZE, MY_PROT_READ | MY_PROT_WRITE, MY_MAP_PRIVATE | MY_MAP_ANONYMOUS, -1, 0);
        
            if(arena == MY_MAP_FAILED) {
                return NULL;
            }

            object_metadata* head = carve_arena(arena, SMALL_ARENA_SIZE, size);
            
            spin_acquire(&small_locks[idx]);
            small_bins[idx] = head;

            for(int i = 0; i < SMALL_BATCH_SIZE; i++) {
                object_metadata* sob = small_bins[idx];

                if(sob == NULL) break;

                small_bins[idx] = sob->next;

                sob->next = tcache->small_bins[idx];
                tcache->small_bins[idx] = sob;
            }

            tcache->small_counts[idx] = SMALL_BATCH_SIZE;

            head->next = IN_USE;

            spin_release(&small_locks[idx]);
            return (void*)(head + 1);
        } 
    }

    //6. Handle allocations 4KB - 128KB
    else if(size <= MID_END) {
        short idx = size / MID_STRIDE - 1; // The increment between objects in the mid_bin is 4kb
        
        if(tcache->mid_bins[idx]) {
            object_metadata* sob = tcache->mid_bins[idx];

            tcache->mid_bins[idx] = sob->next;
            sob->next = IN_USE;

            tcache->mid_counts[idx]--;

            return (void*)(sob + 1);
        }
        

        else {
            spin_acquire(&mid_locks[idx]);
            if(mid_bins[idx]) {
                for(int i = 0; i < MID_BATCH_SIZE; i++) {
                    object_metadata* sob = mid_bins[idx];
                    if(sob == NULL) break;
                    
                    mid_bins[idx] = sob->next;
                    
                    sob->next = tcache->mid_bins[idx];
                    tcache->mid_bins[idx] = sob;
                }
                object_metadata* head = tcache->mid_bins[idx];

                tcache->mid_bins[idx] = head->next;
                head->next = IN_USE;

                tcache->mid_counts[idx] = MID_BATCH_SIZE - 1;

                spin_release(&mid_locks[idx]);
                return (void*)(head + 1);
            }

            spin_release(&mid_locks[idx]);
            void* arena= my_mmap(NULL, MID_ARENA_SIZE, MY_PROT_READ | MY_PROT_WRITE, MY_MAP_PRIVATE | MY_MAP_ANONYMOUS, -1, 0);
            
            if(arena == MY_MAP_FAILED) {
                return NULL;
            }

            object_metadata* head = carve_arena(arena, MID_ARENA_SIZE, size);
            spin_acquire(&mid_locks[idx]);
            mid_bins[idx] = head;

            for(int i = 0; i < MID_BATCH_SIZE; i++) {
                object_metadata* sob = mid_bins[idx];

                if(sob == NULL) break;

                mid_bins[idx] = sob->next;

                sob->next = tcache->mid_bins[idx];
                tcache->mid_bins[idx] = sob;
            }

            tcache->mid_counts[idx] = MID_BATCH_SIZE;
            head->next = IN_USE;

            spin_release(&mid_locks[idx]);
            return (void*)(head + 1);
        }
    }

    return NULL;
}

int my_free(void* ptr) {

    if(ptr == NULL) {
        return -1;
    }
    
    object_metadata* obj = get_object_ptr(ptr);
    if(obj == NULL) {
        return -1;
    }

    if(obj->next != IN_USE){
        return -1;
    }

    if(obj->size > MY_MMAP_TRESHOLD) {
        int ret = my_munmap(obj, sizeof(object_metadata) + obj->size);
        return ret;
    }
    else if (obj->size <= TINY_END) {
        short idx = obj->size / TINY_STRIDE - 1;
        int count = tcache->tiny_counts[idx];

        if(count >= TINY_BATCH_SIZE * 2) {
            spin_acquire(&tiny_locks[idx]);
            for(int i = 0; i < TINY_BATCH_SIZE * 2; i++){
                object_metadata* sob = tcache->tiny_bins[idx];

                tcache->tiny_bins[idx] = sob->next;

                sob->next = tiny_bins[idx];
                tiny_bins[idx] = sob;
            }
            spin_release(&tiny_locks[idx]);
        }

        obj->next = tcache->tiny_bins[idx];
        tcache->tiny_bins[idx] = obj;
    }
    else if (obj->size <= SMALL_END) {
        short idx = obj->size / SMALL_STRIDE - 1;

        int count = tcache->small_counts[idx];
        if(count >= SMALL_BATCH_SIZE * 2) {
            spin_acquire(&small_locks[idx]);
            for(int i = 0; i < SMALL_BATCH_SIZE * 2; i++){
                object_metadata* sob = tcache->small_bins[idx];

                tcache->small_bins[idx] = sob->next;

                sob->next = small_bins[idx];
                small_bins[idx] = sob;
            }
            spin_release(&small_locks[idx]);
        }

        obj->next = tcache->small_bins[idx];
        tcache->small_bins[idx] = obj;
    }
    else if (obj->size <= MID_END) {
        short idx = obj->size / MID_STRIDE - 1;
        
        
        int count = tcache->mid_counts[idx];
        if(count >= MID_BATCH_SIZE * 2) {
            spin_acquire(&mid_locks[idx]);
            for(int i = 0; i < MID_BATCH_SIZE * 2; i++){
                object_metadata* sob = tcache->mid_bins[idx];

                tcache->mid_bins[idx] = sob->next;

                sob->next = mid_bins[idx];
                mid_bins[idx] = sob;
            }
            spin_release(&mid_locks[idx]);
        }

        obj->next = tcache->mid_bins[idx];
        tcache->mid_bins[idx] = obj;
    }
    
    return 0;
}

void* my_calloc(unsigned long nmemb, unsigned long size) {
    unsigned long total_size = nmemb * size;
    if(nmemb == 0 || size == 0 || total_size / size != nmemb) {
        return NULL;
    }
    void* ptr = my_malloc(total_size);

    //1. Optimize for blocks allocated via mmap dirrectly, we can skip them
    object_metadata* obj = get_object_ptr(ptr);
    if(obj->size > MY_MMAP_TRESHOLD) return ptr;

    //2. Zero any other objects
    unsigned long qwords = total_size / 8;
    __asm__ __volatile__(
        "cld\n\t"
        "rep stosq\n\t"
        : 
        : "a"(0) ,"D"(ptr), "c" (qwords)
        :
    );
    return ptr;
}

void* my_realloc(void* ptr, unsigned long new_size) {
    if(ptr == NULL) {
        return my_malloc(new_size);
    }

    if(new_size == 0) {
        my_free(ptr);
        return NULL;
    }

    new_size = ALIGN16(new_size);
    object_metadata* obj = get_object_ptr(ptr);

    if(obj->next != IN_USE) {
        my_free(ptr);
    }

    if(obj->size > MY_MMAP_TRESHOLD) {
        object_metadata* new_obj = my_mremap(obj, sizeof(object_metadata) + obj->size, sizeof(object_metadata) + new_size, MY_MREMAP_MAYMOVE);

        if(new_obj == MY_MAP_FAILED){
            return NULL;
        }

        new_obj->size = new_size;
        new_obj->next = IN_USE;
        return (void*)(obj + 1);
    }

    if(obj->size >= new_size) {
        return ptr;
    }
    void* new_ptr = my_malloc(new_size);

    unsigned long nqwords = obj->size / 8;

    __asm__ __volatile__(
        "cld\n\t"
        "rep movsq\n\t"
        :
        : "D"(new_ptr), "S"(ptr), "c"(nqwords)
        :
    );
    my_free(ptr);
    return new_ptr;
}
