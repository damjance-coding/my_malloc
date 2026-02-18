#// test_my_malloc.c
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>

#define MY_MALLOC_IMPLEMENTATION
#include "my_malloc.h"

#define CHECK(x) do { if(!(x)) { \
    printf("FAILED: %s:%d\n", __FILE__, __LINE__); \
    assert(0); } } while(0)

static void test_basic_allocation() {
    void* p = my_malloc(32);
    CHECK(p != NULL);

    object_metadata* meta = get_object_ptr(p);
    CHECK(meta->size >= 32);
    CHECK(((uintptr_t)p % 16) == 0);

    CHECK(my_free(p) == 0);
}

static void test_zero_malloc() {
    void* p = my_malloc(0);
    CHECK(p == NULL);
}

static void test_double_free() {
    void* p = my_malloc(64);
    CHECK(p != NULL);

    CHECK(my_free(p) == 0);
    CHECK(my_free(p) == -1);  // should detect double free
}

static void test_large_allocation_mmap() {
    void* p = my_malloc(200000); // > 128KB threshold
    CHECK(p != NULL);

    object_metadata* meta = get_object_ptr(p);
    CHECK(meta->size >= 200000);

    CHECK(my_free(p) == 0);
}

static void test_calloc_zeroed() {
    int* arr = my_calloc(16, sizeof(int));
    CHECK(arr != NULL);

    for(int i = 0; i < 16; i++) {
        CHECK(arr[i] == 0);
    }

    CHECK(my_free(arr) == 0);
}

static void test_realloc_grow() {
    int* p = my_malloc(32);
    CHECK(p != NULL);

    for(int i = 0; i < 8; i++) p[i] = i;

    int* q = my_realloc(p, 128);
    CHECK(q != NULL);

    for(int i = 0; i < 8; i++) {
        CHECK(q[i] == i);
    }

    CHECK(my_free(q) == 0);
}

static void test_realloc_shrink() {
    int* p = my_malloc(128);
    CHECK(p != NULL);

    int* q = my_realloc(p, 32);
    CHECK(q == p);  // should not move if shrinking

    CHECK(my_free(q) == 0);
}

static void test_batch_behavior() {
    void* ptrs[128];

    for(int i = 0; i < 128; i++) {
        ptrs[i] = my_malloc(32);
        CHECK(ptrs[i] != NULL);
    }

    for(int i = 0; i < 128; i++) {
        CHECK(my_free(ptrs[i]) == 0);
    }
}

int main() {
    printf("Running tests...\n");

    test_basic_allocation();
    test_zero_malloc();
    test_double_free();
    test_large_allocation_mmap();
    test_calloc_zeroed();
    test_realloc_grow();
    test_realloc_shrink();
    test_batch_behavior();

    printf("All tests passed.\n");
    return 0;
}
