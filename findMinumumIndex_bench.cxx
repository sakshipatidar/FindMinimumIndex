﻿#include <algorithm>
#include <benchmark/benchmark.h>
#include <cstdlib>
#include <cstring>
#include <random>
#include <x86intrin.h>
/*
 * Alignment of  32 bytes
 */
constexpr int alignment = 32;
/*
 * create global data
 * a bit hacky way
 */
constexpr size_t nn = 8 << 10;
float* inArray;
class InitArray
{
public:
  InitArray()
  {
    std::mt19937 gen;
    std::uniform_real_distribution<> dis(1.0, 10.0);
    // create buffer of right size,properly aligned
    size_t const size = nn * sizeof(float);
    posix_memalign((void**)&inArray, alignment, size);
    for (size_t i = 0; i < nn; ++i) {
      // Use dis to transform the random unsigned int generated by gen into a
      // double. Each call to dis(gen) generates a new random double
      inArray[i] = dis(gen);
    }
  }
  ~InitArray() { free(inArray); }
};
InitArray initArray;

/*
 * Test non vector code
 */

/*
 * C style 1
 */
static void
findMinimumIndexC(benchmark::State& state)
{
  for (auto _ : state) {
    const int n = state.range(0);
    float* array = (float*)__builtin_assume_aligned(inArray, alignment);
    float minvalue = array[0];
    int  minIndex = 0;
    for (int i = 0; i < n; ++i) {
      const float value = array[i];
      if (value < minvalue) {
        minvalue = value;
        minIndex = i;
      }
    }
    benchmark::DoNotOptimize(&minIndex);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(findMinimumIndexC)->Range(8, nn);

/*
 * C style 2
 */
static void
findMinimumIndexC2(benchmark::State& state)
{
  for (auto _ : state) {
    const int n = state.range(0);
    float* array = (float*)__builtin_assume_aligned(inArray, alignment);
    int  minIndex = 0;
    for (int i = 0; i < n; ++i) {
      if (array[i] < array[minIndex]) {
        minIndex = i;
      }
    }
    benchmark::DoNotOptimize(&minIndex);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(findMinimumIndexC2)->Range(8, nn);
/*
 * Use STL
 */
static void
findMinimumIndexSTL(benchmark::State& state)
{
  for (auto _ : state) {
    const int n = state.range(0);
    float* array = (float*)__builtin_assume_aligned(inArray, alignment);
    size_t minIndex = std::distance(array, std::min_element(array, array + n));
    benchmark::DoNotOptimize(&minIndex);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(findMinimumIndexSTL)->Range(8, nn);

/*
 * Vectorized versions
 * First specific ones 
 * later will try function
 * multiversioning
 */

/*
 * check for blend
 * as SSE2 does not have one
 */
#if defined(__SSE4_1__)
const auto mm_blendv_epi8 = _mm_blendv_epi8;
#elif defined(__SSE2__)
static inline __m128i
SSE2_mm_blendv_epi8(__m128i a, __m128i b, __m128i mask)
{
  return _mm_or_si128(_mm_andnot_si128(mask, a), _mm_and_si128(mask, b));
}
const auto mm_blendv_epi8 = SSE2_mm_blendv_epi8;
#endif // blend

#if defined(__AVX2__)
/*
 * AVX2 : 8 elements at a time
 */
static void
findMinimumIndexAVX2(benchmark::State& state)
{
  for (auto _ : state) {
    const int n = state.range(0);
    float* array = (float*)__builtin_assume_aligned(inArray, alignment);

    const __m256i increment = _mm256_set1_epi32(8);
    __m256i indices = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    __m256i minindices = indices;
    __m256 minvalues = _mm256_load_ps(array);

    for (int i = 8; i < n; i += 8) {
      /*
       * Load next 8 elements
       */
      const __m256 values = _mm256_load_ps(array + i);
      /*
       * increment the indices
       */
      indices = _mm256_add_epi32(indices, increment);
      /*
       * Get a mask indicating when an element is less than the ones we have
       */
      __m256i lt =
        _mm256_castps_si256(_mm256_cmp_ps(values, minvalues, _CMP_LT_OS));
      /*
       * blend select the indices to update
       */
      minindices = _mm256_blendv_epi8(minindices, indices, lt);
      minvalues = _mm256_min_ps(values, minvalues);
    }
    /*
     * Do the final calculation scalar way
     */
    alignas(alignment) float finalValues[8];
    alignas(alignment) int32_t finalIndices[8];
    _mm256_store_ps(finalValues, minvalues);
    _mm256_store_si256((__m256i*)(finalIndices), minindices);
    size_t minIndex = finalIndices[0];
    float minvalue = finalValues[0];
    for (size_t i = 1; i < 8; ++i) {
      const float value = finalValues[i];
      if (value < minvalue) {
        minvalue = value;
        minIndex = finalIndices[i];
      }
    }
    benchmark::DoNotOptimize(&minIndex);
    benchmark::ClobberMemory();
  }
}

BENCHMARK(findMinimumIndexAVX2)->Range(8, nn);
#endif//end if AVX2


/*
 * SSE2/4.1 : 8 elements at time
 */
#if defined(__SSE2__)
static void
findMinimumIndexSSE(benchmark::State& state)
{
  for (auto _ : state) {
    const int n = state.range(0);
    float* array = (float*)__builtin_assume_aligned(inArray, alignment);

    const __m128i increment = _mm_set1_epi32(8);
    __m128i indices1 = _mm_setr_epi32(0, 1, 2, 3);
    __m128i indices2 = _mm_setr_epi32(4, 5, 6, 7);
    __m128i minindices1 = indices1;
    __m128i minindices2 = indices2;
    __m128 minvalues1 = _mm_load_ps(array);
    __m128 minvalues2 = _mm_load_ps(array + 4);

    for (int i = 8; i < n; i += 8) {
      // Load 8 elements at a time
      const __m128 values1 = _mm_load_ps(array + i);     // first 4
      const __m128 values2 = _mm_load_ps(array + i + 4); // second 4
      // 1
      indices1 = _mm_add_epi32(indices1, increment);
      __m128i lt1 = _mm_castps_si128(_mm_cmplt_ps(values1, minvalues1));
      minindices1 = mm_blendv_epi8(minindices1, indices1, lt1);
      minvalues1 = _mm_min_ps(values1, minvalues1);
      // 2
      indices2 = _mm_add_epi32(indices2, increment);
      __m128i lt2 = _mm_castps_si128(_mm_cmplt_ps(values2, minvalues2));
      minindices2 = mm_blendv_epi8(minindices2, indices2, lt2);
      minvalues2 = _mm_min_ps(values2, minvalues2);
    }
    // Compare //1 with //2
    __m128i lt = _mm_castps_si128(_mm_cmplt_ps(minvalues1, minvalues2));
    minindices1 = mm_blendv_epi8(minindices2, minindices1, lt);
    minvalues1 = _mm_min_ps(minvalues2, minvalues1);
    /*
     * Do the final calculation scalar way
     */
    alignas(alignment) float finalValues[4];
    alignas(alignment) int32_t finalIndices[4];
    _mm_store_ps(finalValues, minvalues1);
    _mm_store_si128((__m128i*)(finalIndices), minindices1);
    size_t minIndex = finalIndices[0];
    float minvalue = finalValues[0];
    for (size_t i = 1; i < 4; ++i) {
      const float value = finalValues[i];
      if (value < minvalue) {
        minvalue = value;
        minIndex = finalIndices[i];
      }
    }
    benchmark::DoNotOptimize(&minIndex);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(findMinimumIndexSSE)->Range(8, nn);
#endif // SSE2/4.1

/*
 * Implementation of the abive but now 
 * using function multiversioning
 */
#if defined(__ELF__) || defined(__SSE2__)
__attribute__((target("sse4.2,sse2"))) static void
findMinimumIndexDispatch(benchmark::State& state)
{
  for (auto _ : state) {
    const int n = state.range(0);
    float* array = (float*)__builtin_assume_aligned(inArray, alignment);

    const __m128i increment = _mm_set1_epi32(8);
    __m128i indices1 = _mm_setr_epi32(0, 1, 2, 3);
    __m128i indices2 = _mm_setr_epi32(4, 5, 6, 7);
    __m128i minindices1 = indices1;
    __m128i minindices2 = indices2;
    __m128 minvalues1 = _mm_load_ps(array);
    __m128 minvalues2 = _mm_load_ps(array + 4);

    for (int i = 8; i < n; i += 8) {
      // Load 8 elements at a time
      const __m128 values1 = _mm_load_ps(array + i);     // first 4
      const __m128 values2 = _mm_load_ps(array + i + 4); // second 4
      // 1
      indices1 = _mm_add_epi32(indices1, increment);
      __m128i lt1 = _mm_castps_si128(_mm_cmplt_ps(values1, minvalues1));
      minindices1 = mm_blendv_epi8(minindices1, indices1, lt1);
      minvalues1 = _mm_min_ps(values1, minvalues1);
      // 2
      indices2 = _mm_add_epi32(indices2, increment);
      __m128i lt2 = _mm_castps_si128(_mm_cmplt_ps(values2, minvalues2));
      minindices2 = mm_blendv_epi8(minindices2, indices2, lt2);
      minvalues2 = _mm_min_ps(values2, minvalues2);
    }
    // Compare //1 with //2
    __m128i lt = _mm_castps_si128(_mm_cmplt_ps(minvalues1, minvalues2));
    minindices1 = mm_blendv_epi8(minindices2, minindices1, lt);
    minvalues1 = _mm_min_ps(minvalues2, minvalues1);
    /*
     * Do the final calculation scalar way
     */
    alignas(alignment) float finalValues[4];
    alignas(alignment) int32_t finalIndices[4];
    _mm_store_ps(finalValues, minvalues1);
    _mm_store_si128((__m128i*)(finalIndices), minindices1);
    size_t minIndex = finalIndices[0];
    float minvalue = finalValues[0];
    for (size_t i = 1; i < 4; ++i) {
      const float value = finalValues[i];
      if (value < minvalue) {
        minvalue = value;
        minIndex = finalIndices[i];
      }
    }
    benchmark::DoNotOptimize(&minIndex);
    benchmark::ClobberMemory();
  }
}
#endif

#if defined(__ELF__) || defined(__AVX2__)
__attribute__((target("avx2"))) static void
findMinimumIndexDispatch(benchmark::State& state)
{
  for (auto _ : state) {
    const int n = state.range(0);
    float* array = (float*)__builtin_assume_aligned(inArray, alignment);

    const __m256i increment = _mm256_set1_epi32(8);
    __m256i indices = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    __m256i minindices = indices;
    __m256 minvalues = _mm256_load_ps(array);

    for (int i = 8; i < n; i += 8) {
      /*
       * Load next 8 elements
       */
      const __m256 values = _mm256_load_ps(array + i);
      /*
       * increment the indices
       */
      indices = _mm256_add_epi32(indices, increment);
      /*
       * Get a mask indicating when an element is less than the ones we have
       */
      __m256i lt =
        _mm256_castps_si256(_mm256_cmp_ps(values, minvalues, _CMP_LT_OS));
      /*
       * blend select the indices to update
       */
      minindices = _mm256_blendv_epi8(minindices, indices, lt);
      minvalues = _mm256_min_ps(values, minvalues);
    }
    /*
     * Do the final calculation scalar way
     */
    alignas(alignment) float finalValues[8];
    alignas(alignment) int32_t finalIndices[8];
    _mm256_store_ps(finalValues, minvalues);
    _mm256_store_si256((__m256i*)(finalIndices), minindices);
    size_t minIndex = finalIndices[0];
    float minvalue = finalValues[0];
    for (size_t i = 1; i < 8; ++i) {
      const float value = finalValues[i];
      if (value < minvalue) {
        minvalue = value;
        minIndex = finalIndices[i];
      }
    }
    benchmark::DoNotOptimize(&minIndex);
    benchmark::ClobberMemory();
  }
}
#endif
__attribute__((target("default"))) static void
findMinimumIndexDispatch(benchmark::State& state)
{
  for (auto _ : state) {
    const int n = state.range(0);
    float* array = (float*)__builtin_assume_aligned(inArray, alignment);
    float minvalue = array[0];
    size_t minIndex = 0;
    for (int i = 0; i < n; ++i) {
      const float value = array[i];
      if (value < minvalue) {
        minvalue = value;
        minIndex = i;
      }
    }
    benchmark::DoNotOptimize(&minIndex);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(findMinimumIndexDispatch)->Range(8, nn);
//Done with multiversioning


BENCHMARK_MAIN();

