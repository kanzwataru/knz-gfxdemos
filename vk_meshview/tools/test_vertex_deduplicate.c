/* 
 * Just a test program to see how long it would take to
 * deduplicate verts in C rather than Python (in which it's excruciatingly slow).
 * 
 * To make it (un)fair, we take in mesh data that is potentially already deduplicated,
 * then unpack the vertices and then merge them again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>

#define WITH_SIMD 0
#define WITH_THRESHOLD 0

#if WITH_SIMD
    #include <immintrin.h>
#endif

static char scratch_mem[256 * 1024 * 1024];
char *scratch_mem_top = scratch_mem;

static void *align_ptr(void *addr, uint64_t alignment)
{
    return (void *)(((uint64_t)addr + alignment - 1) & ~(alignment - 1));
}

static void *push_memory(size_t size)
{
    //assert((uint64_t)(scratch_mem_top + size + 16) - (uint64_t)scratch_mem < sizeof(scratch_mem));

    void *ptr = align_ptr(scratch_mem_top, 32);
    scratch_mem_top += size;

    return ptr;
}

#define THRESHOLD 0.0001f

static inline bool mostly_equal(float a, float b)
{
#if WITH_THRESHOLD
    return ((a - b) < THRESHOLD) && ((b - a) < THRESHOLD);
#else
    return a == b;
#endif
}

#if WITH_SIMD
static const __m256 c_threshold = {THRESHOLD, THRESHOLD, THRESHOLD, THRESHOLD, THRESHOLD, THRESHOLD, THRESHOLD, THRESHOLD};

static inline __m256 mostly_equal_avx(__m256 a, __m256 b)
{
#if WITH_THRESHOLD
    __m256 first = _mm256_cmp_ps(_mm256_sub_ps(a, b), c_threshold, _CMP_LE_OQ);
    __m256 second = _mm256_cmp_ps(_mm256_sub_ps(b, a), c_threshold, _CMP_LE_OQ);

    return _mm256_and_ps(first, second);
#else
    __m256 mask = _mm256_cmp_ps(a, b, _CMP_EQ_OQ);
    
    return mask;
#endif
}
#endif

int main(int argc, char **argv)
{  
    /* Load in existing mesh data */
    printf("Loading in data...\n");
    FILE *fp = fopen(argv[1], "rb");

    uint32_t vert_count;
    uint32_t index_count;

    fread(&vert_count, sizeof(vert_count), 1, fp);
    fread(&index_count, sizeof(index_count), 1, fp);

    uint32_t vert_elem_count = 8;
    uint32_t vert_size_bytes = vert_elem_count * sizeof(float);
    
    uint32_t vert_buffer_size = vert_count * vert_size_bytes;
    uint32_t index_buffer_size = index_count * sizeof(uint16_t);

    float *vert_buffer = push_memory(vert_buffer_size);
    uint16_t *index_buffer = push_memory(index_buffer_size);
    
    fread(vert_buffer, vert_buffer_size, 1, fp);
    fread(index_buffer, index_buffer_size, 1, fp);

    fclose(fp);
    
    /* Unpack vertices */
    printf("Unpacking vertices...\n");
    uint32_t max_vert_buffer_size = index_count * vert_size_bytes;
    float *vert_buffer_unpacked = push_memory(max_vert_buffer_size);
    
    uint32_t unpacked_vert_count = 0;
    
    for(uint32_t i = 0; i < index_count; ++i) {
        uint16_t index = index_buffer[i];

        memcpy(&vert_buffer_unpacked[vert_elem_count * unpacked_vert_count++],
               &vert_buffer[index * vert_elem_count],
               vert_size_bytes);
    }
    
    assert(unpacked_vert_count == index_count);

    /* Re-deduplicate vertices */
    printf("Deduplicating vertices...\n");
    float *new_vertex_buffer = push_memory(max_vert_buffer_size);
    uint16_t *new_index_buffer = push_memory(index_buffer_size);

    uint32_t new_index_buffer_count = 0;
    uint32_t new_vertex_buffer_count = 0;

#if 1
    for(uint32_t i = 0; i < unpacked_vert_count; ++i) {
#if WITH_SIMD
        __m256 vert = _mm256_load_ps(&vert_buffer_unpacked[i * 8]);
#endif
        
        uint32_t found_index;
        bool found = false;
        for(uint32_t j = 0; j < new_vertex_buffer_count; ++j) {
#if WITH_SIMD
            __m256 new_vert = _mm256_load_ps(&new_vertex_buffer[j * 8]);
            //__m256 mask = _mm256_cmp_ps(new_vert, vert, _CMP_EQ_OQ);
            __m256 mask = mostly_equal_avx(new_vert, vert);
            int i_mask = _mm256_movemask_ps(mask);
            
            if(i_mask == 0xFF) {
                found_index = j;
                found = true;
                break;
            }
#else
            if(mostly_equal(new_vertex_buffer[j * 8 + 0], vert_buffer_unpacked[i * 8 + 0]) &&
               mostly_equal(new_vertex_buffer[j * 8 + 1], vert_buffer_unpacked[i * 8 + 1]) &&
               mostly_equal(new_vertex_buffer[j * 8 + 2], vert_buffer_unpacked[i * 8 + 2]) &&
               mostly_equal(new_vertex_buffer[j * 8 + 3], vert_buffer_unpacked[i * 8 + 3]) &&
               mostly_equal(new_vertex_buffer[j * 8 + 4], vert_buffer_unpacked[i * 8 + 4]) &&
               mostly_equal(new_vertex_buffer[j * 8 + 5], vert_buffer_unpacked[i * 8 + 5]) &&
               mostly_equal(new_vertex_buffer[j * 8 + 6], vert_buffer_unpacked[i * 8 + 6]) &&
               mostly_equal(new_vertex_buffer[j * 8 + 7], vert_buffer_unpacked[i * 8 + 7])
            ) {
                found_index = j;
                found = true;
                break;
            }
#endif
        }
        
        if(found) {
            new_index_buffer[new_index_buffer_count++] = found_index;
        }
        else {
            new_index_buffer[new_index_buffer_count++] = new_vertex_buffer_count;
            
            memcpy(&new_vertex_buffer[vert_elem_count * new_vertex_buffer_count++],
                   &vert_buffer_unpacked[vert_elem_count * i],
                   vert_size_bytes);
        }
    }
#else
    for(uint32_t i = 0; i < unpacked_vert_count; ++i) {
        new_index_buffer[new_index_buffer_count++] = new_vertex_buffer_count;
            
        memcpy(&new_vertex_buffer[vert_elem_count * new_vertex_buffer_count++],
                &vert_buffer_unpacked[vert_elem_count * i],
                vert_size_bytes);
    }
#endif

    printf("Writing out data...\n");
    fp = fopen("test.bin", "wb");
    fwrite(&new_vertex_buffer_count, sizeof(new_vertex_buffer_count), 1, fp);
    fwrite(&new_index_buffer_count, sizeof(new_index_buffer_count), 1, fp);
    fwrite(new_vertex_buffer, new_vertex_buffer_count * vert_size_bytes, 1, fp);
    fwrite(new_index_buffer, new_index_buffer_count * sizeof(uint16_t), 1, fp);
    fclose(fp);

    printf("Done. Stats:\n");
    printf("\tOriginal vert count: %d\n", vert_count);
    printf("\tOriginal index count: %d\n", index_count);
    printf("\tNew vert count: %d\n", new_vertex_buffer_count);
    printf("\tNew index count: %d\n", new_index_buffer_count);

    return 0;
}
