#include "hip/hip_runtime.h"
/*
 * Copyright 1993-2009 NVIDIA Corporation.  All rights reserved.
 *
 * NVIDIA Corporation and its licensors retain all intellectual property and 
 * proprietary rights in and to this software and related documentation and 
 * any modifications thereto.  Any use, reproduction, disclosure, or distribution 
 * of this software and related documentation without an express license 
 * agreement from NVIDIA Corporation is strictly prohibited.
 * 
 */

/* Matrix multiplication: C = A * B.
 * Host code.
 *
 * This sample implements matrix multiplication and is exactly the same as
 * Chapter 7 of the programming guide.
 * It has been written for clarity of exposition to illustrate various CUDA
 * programming principles, not with the goal of providing the most
 * performant generic kernel for matrix multiplication.
 *
 * CUBLAS provides high-performance matrix multiplication.
 */

// includes, system
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

// includes, project
//#include <cutil_inline.h>
//#include <helper_functions.h>

// includes, kernels
#include <matrixMul_kernel.cuh>
#include <matrixMul_naive.cuh>
#include <matrixMul_tiling.cuh>
#include <matrixMul_coalescing.cuh>
#include <matrixMul_noBankConflict.cuh>
#include <matrixMul_compOpt.cuh>
#include <matrixMul_unroll.cuh>
#include <matrixMul_prefetch.cuh>

////////////////////////////////////////////////////////////////////////////////
// declaration, forward
void runTest(int argc, char** argv);
void randomInit(float*, int);
void printDiff(float*, float*, int, int);

extern "C"
void computeGold(float*, const float*, const float*, unsigned int, unsigned int, unsigned int);

////////////////////////////////////////////////////////////////////////////////
// Helper Functions
////////////////////////////////////////////////////////////////////////////////
#ifndef STRNCASECMP
#define STRNCASECMP strncasecmp
#endif

inline int stringRemoveDelimiter(char delimiter, const char *string)
{
    int string_start = 0;

    while (string[string_start] == delimiter)
    {
        string_start++;
    }

    if (string_start >= (int)strlen(string)-1)
    {
        return 0;
    }

    return string_start;
}

inline bool checkCmdLineFlag(const int argc, const char **argv, const char *string_ref)
{
    bool bFound = false;

    if (argc >= 1)
    {
        for (int i=1; i < argc; i++)
        {
            int string_start = stringRemoveDelimiter('-', argv[i]);
            const char *string_argv = &argv[i][string_start];

            const char *equal_pos = strchr(string_argv, '=');
            int argv_length = (int)(equal_pos == 0 ? strlen(string_argv) : equal_pos - string_argv);

            int length = (int)strlen(string_ref);

            if (length == argv_length && !STRNCASECMP(string_argv, string_ref, length))
            {
                bFound = true;
                continue;
            }
        }
    }

    return bFound;
}

inline int getCmdLineArgumentInt(const int argc, const char **argv, const char *string_ref)
{
    bool bFound = false;
    int value = -1;

    if (argc >= 1)
    {
        for (int i=1; i < argc; i++)
        {
            int string_start = stringRemoveDelimiter('-', argv[i]);
            const char *string_argv = &argv[i][string_start];
            int length = (int)strlen(string_ref);

            if (!STRNCASECMP(string_argv, string_ref, length))
            {
                if (length+1 <= (int)strlen(string_argv))
                {
                    int auto_inc = (string_argv[length] == '=') ? 1 : 0;
                    value = atoi(&string_argv[length + auto_inc]);
                }
                else
                {
                    value = 0;
                }

                bFound = true;
                continue;
            }
        }
    }

    if (bFound)
    {
        return value;
    }
    else
    {
        return 0;
    }
}

void TransposeMatrix(float *data, float *dataT, int size, int wB, int hB){
  for (int i = 0; i < wB; i++){
    for (int j = 0; j < hB; j++){
      dataT[hB*i + j] = data[wB*j + i];
    }
  }

}


////////////////////////////////////////////////////////////////////////////////
// Program main
////////////////////////////////////////////////////////////////////////////////
int
main(int argc, char** argv)
{
    runTest(argc, argv);

    exit(EXIT_SUCCESS);
}

////////////////////////////////////////////////////////////////////////////////
//! Run a simple test for CUDA
////////////////////////////////////////////////////////////////////////////////
void
runTest(int argc, char** argv)
{

    /****************************************************/
    /*  Preparations                                    */
    /****************************************************/

    printf("[Matrix Multiply Using CUDA] - Starting...\n");

    if (checkCmdLineFlag(argc, (const char **)argv, "help") ||
        checkCmdLineFlag(argc, (const char **)argv, "?"))
    {
        printf("Usage -device=n (n >= 0 for deviceID)\n");
        printf("      -wA=WidthA -hA=HeightA (Width x Height of Matrix A)\n");
        printf("      -wB=WidthB -hB=HeightB (Width x Height of Matrix B)\n");
        printf("  Note: Outer matrix dimensions of A & B matrices must be equal.\n");

        exit(EXIT_SUCCESS);
    }

    // By default, we use device 0, otherwise we override the device ID based on what is provided at the command line
    int devID = 0;

    if (checkCmdLineFlag(argc, (const char **)argv, "device"))
    {
        devID = getCmdLineArgumentInt(argc, (const char **)argv, "device");
        hipSetDevice(devID);
    }

    int WA = (32 * BLOCK_SIZE); // Matrix A width
    int HA = (16 * BLOCK_SIZE); // Matrix A height
    int WB = (24 * BLOCK_SIZE); // Matrix B width
    int HB = WA;  // Matrix B height


    printf("%d, %d, %d, %d\n",WA, HA, WB, HB);

    if (checkCmdLineFlag(argc, (const char **)argv, "wA"))
    {
        WA = getCmdLineArgumentInt(argc, (const char **)argv, "wA");
    }

    if (checkCmdLineFlag(argc, (const char **)argv, "hA"))
    {
        HA = getCmdLineArgumentInt(argc, (const char **)argv, "hA");
    }

    if (checkCmdLineFlag(argc, (const char **)argv, "wB"))
    {
        WB = getCmdLineArgumentInt(argc, (const char **)argv, "wB");
    }

    if (checkCmdLineFlag(argc, (const char **)argv, "hB"))
    {
        HB = getCmdLineArgumentInt(argc, (const char **)argv, "hB");
    }

    

    printf("%d, %d, %d, %d\n",WA, HA, WB, HB);

    int WC = WB;  // Matrix C width 
    int HC = HA;  // Matrix C height

    hipError_t error;
    hipDeviceProp_t deviceProp;
    error = hipGetDevice(&devID);

    if (error != hipSuccess)
    {
        printf("hipGetDevice returned error code %d, line(%d)\n", error, __LINE__);
    }

    error = hipGetDeviceProperties(&deviceProp, devID);

    if (deviceProp.computeMode == hipComputeModeProhibited)
    {
        fprintf(stderr, "Error: device is running in <Compute Mode Prohibited>, no threads can use ::hipSetDevice().\n");
        exit(EXIT_SUCCESS);
    }

    if (error != hipSuccess)
    {
        printf("hipGetDeviceProperties returned error code %d, line(%d)\n", error, __LINE__);
    }
    else
    {
        printf("GPU Device %d: \"%s\" with compute capability %d.%d\n\n", devID, deviceProp.name, deviceProp.major, deviceProp.minor);
    }

    // utilities
    hipEvent_t start;
    hipEvent_t stop;
    float msecTotal;

    // set seed for rand()
    srand(2006);

    // allocate host memory for matrices A and B
    unsigned int size_A = WA * HA;
    unsigned int mem_size_A = sizeof(float) * size_A;
    float* h_A = (float*) malloc(mem_size_A);
    unsigned int size_B = WB * HB;
    unsigned int mem_size_B = sizeof(float) * size_B;
    float* h_B = (float*) malloc(mem_size_B);
    float flop = 2 * (float)WC * (float)HC * (float)WA;

    float* h_Bt = (float*) malloc(mem_size_B);


    // initialize host memory
    randomInit(h_A, size_A);
    randomInit(h_B, size_B);

    TransposeMatrix(h_B, h_Bt, size_B, WB, HB);
    
    // allocate device memory
    float* d_A;
    hipMalloc((void**) &d_A, mem_size_A);
    float* d_B;
    hipMalloc((void**) &d_B, mem_size_B);

    // allocate device memory for result
    unsigned int size_C = WC * HC;
    unsigned int mem_size_C = sizeof(float) * size_C;
    float* d_C;
    hipMalloc((void**) &d_C, mem_size_C);

    // allocate host memory for the result
    float* h_C = (float*) malloc(mem_size_C);

#if CHECK_RESULT == 1
    // create and start timer
    hipEventCreate(&start);
    hipEventRecord(start, NULL); 
    // compute reference solution
    float* reference = (float*) malloc(mem_size_C);
    computeGold(reference, h_A, h_B, HA, WA, WB);
    // stop and destroy timer
    hipEventCreate(&stop);
    hipEventRecord(stop, NULL);
    hipEventSynchronize(stop);
    hipEventElapsedTime(&msecTotal, start, stop);
    printf("Naive CPU (Golden Reference)\n");
    printf("Processing time: %f (ms), GFLOPS: %f \n", msecTotal, flop / msecTotal/ 1e+6);
    printf("-------------------------------------\n");
#endif

    dim3 threads,grid;

    /****************************************************/
    /*  CUDA SDK example                                */
    /****************************************************/

    // create and start timer
    hipEventCreate(&start);
    hipEventRecord(start, NULL);
    // copy host memory to device
    hipMemcpy(d_A, h_A, mem_size_A,
                              hipMemcpyHostToDevice);
    hipMemcpy(d_B, h_B, mem_size_B,
                              hipMemcpyHostToDevice);
   // setup execution parameters
    threads = dim3(BLOCK_SIZE, BLOCK_SIZE);
    grid = dim3(WC / threads.x, HC / threads.y);
    // execute the kernel
    hipLaunchKernelGGL(matrixMul, dim3(grid), dim3(threads ), 0, 0, d_C, d_A, d_B, WA, WB);
    // copy result from device to host
    hipMemcpy(h_C, d_C, mem_size_C,
                              hipMemcpyDeviceToHost);
    // stop and destroy timer
    hipEventCreate(&stop);
    hipEventRecord(stop, NULL);
    hipEventSynchronize(stop);
    hipEventElapsedTime(&msecTotal, start, stop);
    printf("GPU SDK Sample\n");
    printf("Processing time: %f (ms), GFLOPS: %f \n", msecTotal, flop / msecTotal/ 1e+6);
    printf("-------------------------------------\n");
#if CHECK_RESULT == 1
    // check result
    printDiff(reference, h_C, WC, HC);
#endif

    /****************************************************/
    /*  naive implementation on GPU                     */
    /****************************************************/

#if ENABLE_NAIVE == 1

    // create and start timer
    hipEventCreate(&start);
    hipEventRecord(start, NULL);
    // setup execution parameters
    threads = dim3(BLOCK_SIZE, BLOCK_SIZE);
    grid = dim3(WC / threads.x, HC / threads.y);
    // copy host memory to device
    hipMemcpy(d_A, h_A, mem_size_A,
                              hipMemcpyHostToDevice);
    hipMemcpy(d_B, h_B, mem_size_B,
                              hipMemcpyHostToDevice);
    // naive implementation
    hipLaunchKernelGGL(matrixMul_naive, dim3(grid), dim3(threads ), 0, 0, d_C, d_A, d_B, WA, WB);
    // copy result from device to host
    hipMemcpy(h_C, d_C, mem_size_C,
                              hipMemcpyDeviceToHost);
    // stop and destroy timer
    hipEventCreate(&stop);
    hipEventRecord(stop, NULL);
    hipEventSynchronize(stop);
    hipEventElapsedTime(&msecTotal, start, stop);
    printf("Naive GPU\n");
    printf("Processing time: %f (ms), GFLOPS: %f \n", msecTotal, flop / msecTotal/ 1e+6);
    printf("-------------------------------------\n");
#if CHECK_RESULT == 1
    // check result
    printDiff(reference, h_C, WC, HC);
#endif

#endif

#if ENABLE_NAIVE == 1

    // create and start timer
    hipEventCreate(&start);
    hipEventRecord(start, NULL);
    // setup execution parameters
    threads = dim3(BLOCK_SIZE, BLOCK_SIZE);
    grid = dim3(WC / threads.x, HC / threads.y);
    // copy host memory to device
    hipMemcpy(d_A, h_A, mem_size_A,
                              hipMemcpyHostToDevice);
    hipMemcpy(d_B, h_Bt, mem_size_B,
                              hipMemcpyHostToDevice);
    // naive implementation
    hipLaunchKernelGGL(matrixMul_naive2, dim3(grid), dim3(threads ), 0, 0, d_C, d_A, d_B, WA, HB);
    // copy result from device to host
    hipMemcpy(h_C, d_C, mem_size_C,
                              hipMemcpyDeviceToHost);
    // stop and destroy timer
    hipEventCreate(&stop);
    hipEventRecord(stop, NULL);
    hipEventSynchronize(stop);
    hipEventElapsedTime(&msecTotal, start, stop);
    printf("Naive GPU using Bt\n");
    printf("Processing time: %f (ms), GFLOPS: %f \n", msecTotal, flop / msecTotal/ 1e+6);
    printf("-------------------------------------\n");
#if CHECK_RESULT == 1
    // check result
    printDiff(reference, h_C, WC, HC);
#endif

#endif

    /****************************************************/
    /*  Tiling without global mem coalescing            */
    /****************************************************/

    // create and start timer
    hipEventCreate(&start);
    hipEventRecord(start, NULL);
    // setup execution parameters
    threads = dim3(BLOCK_SIZE, BLOCK_SIZE);
    grid = dim3(WC / threads.x, HC / threads.y);
    // copy host memory to device
    hipMemcpy(d_A, h_A, mem_size_A,
                              hipMemcpyHostToDevice);
    hipMemcpy(d_B, h_B, mem_size_B,
                              hipMemcpyHostToDevice);
    // naive implementation
    hipLaunchKernelGGL(matrixMul_tiling, dim3(grid), dim3(threads ), 0, 0, d_C, d_A, d_B, WA, WB);
    // copy result from device to host
    hipMemcpy(h_C, d_C, mem_size_C,
                              hipMemcpyDeviceToHost);
    // stop and destroy timer
    hipEventCreate(&stop);
    hipEventRecord(stop, NULL);
    hipEventSynchronize(stop);
    hipEventElapsedTime(&msecTotal, start, stop);
    printf("Tiling GPU\n");
    printf("Processing time: %f (ms), GFLOPS: %f \n", msecTotal, flop / msecTotal/ 1e+6);
    printf("-------------------------------------\n");
#if CHECK_RESULT == 1
    // check result
    printDiff(reference, h_C, WC, HC);
#endif

    /****************************************************/
    /*  Global mem coalescing with smem bank conflict   */
    /****************************************************/

    // create and start timer
    hipEventCreate(&start);
    hipEventRecord(start, NULL); 
    // setup execution parameters
    threads = dim3(BLOCK_SIZE, BLOCK_SIZE);
    grid = dim3(WC / threads.x, HC / threads.y);
    // copy host memory to device
    hipMemcpy(d_A, h_A, mem_size_A,
                              hipMemcpyHostToDevice);
    hipMemcpy(d_B, h_B, mem_size_B,
                              hipMemcpyHostToDevice);
    // naive implementation
    hipLaunchKernelGGL(matrixMul_coalescing, dim3(grid), dim3(threads ), 0, 0, d_C, d_A, d_B, WA, WB);
    // copy result from device to host
    hipMemcpy(h_C, d_C, mem_size_C,
                              hipMemcpyDeviceToHost);
    // stop and destroy timer
    hipEventCreate(&stop);
    hipEventRecord(stop, NULL);
    hipEventSynchronize(stop);
    hipEventElapsedTime(&msecTotal, start, stop);
    printf("Global mem coalescing GPU\n");
    printf("Processing time: %f (ms), GFLOPS: %f \n", msecTotal, flop / msecTotal/ 1e+6);
    printf("-------------------------------------\n");
#if CHECK_RESULT == 1
    // check result
    printDiff(reference, h_C, WC, HC);
#endif

    /****************************************************/
    /*  Global mem coalescing w/o smem bank conflict    */
    /****************************************************/

    // create and start timer
    hipEventCreate(&start);
    hipEventRecord(start, NULL); 
    // setup execution parameters
    threads = dim3(BLOCK_SIZE, BLOCK_SIZE);
    grid = dim3(WC / threads.x, HC / threads.y);
    // copy host memory to device
    hipMemcpy(d_A, h_A, mem_size_A,
                              hipMemcpyHostToDevice);
    hipMemcpy(d_B, h_B, mem_size_B,
                              hipMemcpyHostToDevice);
    // naive implementation
    hipLaunchKernelGGL(matrixMul_noBankConflict, dim3(grid), dim3(threads ), 0, 0, d_C, d_A, d_B, WA, WB);
    // copy result from device to host
    hipMemcpy(h_C, d_C, mem_size_C,
                              hipMemcpyDeviceToHost);
    // stop and destroy timer
    hipEventCreate(&stop);
    hipEventRecord(stop, NULL);
    hipEventSynchronize(stop);
    hipEventElapsedTime(&msecTotal, start, stop);
    printf("Remove shared mem bank conflict GPU\n");
    printf("Processing time: %f (ms), GFLOPS: %f \n", msecTotal, flop / msecTotal/ 1e+6);
    printf("-------------------------------------\n");
#if CHECK_RESULT == 1
    // check result
    printDiff(reference, h_C, WC, HC);
#endif

    /****************************************************/
    /*  Threads perform computation optimizatin         */
    /****************************************************/

    // create and start timer
    hipEventCreate(&start);
    hipEventRecord(start, NULL); 
    // setup execution parameters
    threads = dim3(BLOCK_SIZE, 4);
    grid = dim3(WC / (BLOCK_SIZE*4), HC / BLOCK_SIZE);
    // copy host memory to device
    hipMemcpy(d_A, h_A, mem_size_A,
                              hipMemcpyHostToDevice);
    hipMemcpy(d_B, h_B, mem_size_B,
                              hipMemcpyHostToDevice);
    // naive implementation
    hipLaunchKernelGGL(matrixMul_compOpt, dim3(grid), dim3(threads ), 0, 0, d_C, d_A, d_B, WA, WB);
    // copy result from device to host
    hipMemcpy(h_C, d_C, mem_size_C,
                              hipMemcpyDeviceToHost);
    // stop and destroy timer
    hipEventCreate(&stop);
    hipEventRecord(stop, NULL);
    hipEventSynchronize(stop);
    hipEventElapsedTime(&msecTotal, start, stop);
    printf("Threads perform computation optimization GPU\n");
    printf("Processing time: %f (ms), GFLOPS: %f \n", msecTotal, flop / msecTotal/ 1e+6);
    printf("-------------------------------------\n");
#if CHECK_RESULT == 1
    // check result
    printDiff(reference, h_C, WC, HC);
#endif


    /****************************************************/
    /*  Loop Unrolling                                  */
    /****************************************************/

    // create and start timer
    hipEventCreate(&start);
    hipEventRecord(start, NULL); 
    // setup execution parameters
    threads = dim3(BLOCK_SIZE, 4);
    grid = dim3(WC / (BLOCK_SIZE*4), HC / BLOCK_SIZE);
    // copy host memory to device
    hipMemcpy(d_A, h_A, mem_size_A,
                              hipMemcpyHostToDevice);
    hipMemcpy(d_B, h_B, mem_size_B,
                              hipMemcpyHostToDevice);
    // naive implementation
    hipLaunchKernelGGL(matrixMul_unroll, dim3(grid), dim3(threads ), 0, 0, d_C, d_A, d_B, WA, WB);
    // copy result from device to host
    hipMemcpy(h_C, d_C, mem_size_C,
                              hipMemcpyDeviceToHost);
    // stop and destroy timer
    hipEventCreate(&stop);
    hipEventRecord(stop, NULL);
    hipEventSynchronize(stop);
    hipEventElapsedTime(&msecTotal, start, stop);
    printf("Loop unrolling GPU\n");
    printf("Processing time: %f (ms), GFLOPS: %f \n", msecTotal, flop / msecTotal/ 1e+6);
    printf("-------------------------------------\n");
#if CHECK_RESULT == 1
    // check result
    printDiff(reference, h_C, WC, HC);
#endif

    /****************************************************/
    /*  Prefetching                                     */
    /****************************************************/

    // create and start timer
    hipEventCreate(&start);
    hipEventRecord(start, NULL); 
    // setup execution parameters
    threads = dim3(BLOCK_SIZE, 4);
    grid = dim3(WC / (BLOCK_SIZE*4), HC / BLOCK_SIZE);
    // copy host memory to device
    hipMemcpy(d_A, h_A, mem_size_A,
                              hipMemcpyHostToDevice);
    hipMemcpy(d_B, h_B, mem_size_B,
                              hipMemcpyHostToDevice);
    // naive implementation
    hipLaunchKernelGGL(matrixMul_prefetch, dim3(grid), dim3(threads ), 0, 0, d_C, d_A, d_B, WA, WB);
    // copy result from device to host
    hipMemcpy(h_C, d_C, mem_size_C,
                              hipMemcpyDeviceToHost);
    // stop and destroy timer
    hipEventCreate(&stop);
    hipEventRecord(stop, NULL);
    hipEventSynchronize(stop);
    hipEventElapsedTime(&msecTotal, start, stop);
    printf("Prefetching GPU\n");
    printf("Processing time: %f (ms), GFLOPS: %f \n", msecTotal, flop / msecTotal/ 1e+6);
    printf("-------------------------------------\n");
#if CHECK_RESULT == 1
    // check result
    printDiff(reference, h_C, WC, HC);
#endif

    /****************************************************/
    /*  Cleaning                                        */
    /****************************************************/

    // clean up memory
    free(h_A);
    free(h_B);
    free(h_C);
    free(h_Bt);
#if CHECK_RESULT == 1
    free(reference);
#endif
    hipFree(d_A);
    hipFree(d_B);
    hipFree(d_C);

    hipDeviceReset();
}

// Allocates a matrix with random float entries.
void randomInit(float* data, int size)
{
    for (int i = 0; i < size; ++i)
        data[i] = rand() / (float)RAND_MAX;
}

void printDiff(float *data1, float *data2, int width, int height)
{
  int i,j,k;
  int error_count=0;
  for (j=0; j<height; j++) {
    for (i=0; i<width; i++) {
      k = j*width+i;
      if (fabs(data1[k] - data2[k]) > 0.1 ) {
         printf("diff(%d,%d) CPU=%4.4f, GPU=%4.4f \n", i,j, data1[k], data2[k]);
         error_count++;
      }
    }
  }
  printf("Total Errors = %d \n", error_count);
}
