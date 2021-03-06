#include "hip/hip_runtime.h"
#include <hip/hip_runtime.h>
#include <stdio.h>
#include "include/LBMkernels.cuh"
#include "include/utils.cuh"
#include "include/SWE.cuh"
#include "include/PDEfeq.cuh"
#include "include/BC.cuh"
#include "../include/structs.h"
#include "../include/macros.h"
 
__device__ void calculateMacroscopic(prec* localMacroscopic, prec* localf, prec e, int i){
	localMacroscopic[3*i] = localf[9*i] + (localf[9*i+1] + localf[9*i+2] + localf[9*i+3] + localf[9*i+4]) + (localf[9*i+5] + localf[9*i+6] + localf[9*i+7] + localf[9*i+8]);
	localMacroscopic[3*i+1] = e * ((localf[9*i+1] - localf[9*i+3]) + (localf[9*i+5] - localf[9*i+6] - localf[9*i+7] + localf[9*i+8])) / localMacroscopic[3*i];
	localMacroscopic[3*i+2] = e * ((localf[9*i+2] - localf[9*i+4]) + (localf[9*i+5] + localf[9*i+6] - localf[9*i+7] - localf[9*i+8])) / localMacroscopic[3*i];
}

__device__ void calculateFeqSWE(prec* feq, prec* localMacroscopic, prec e){	
	prec factor = 1 / (9 * e*e);	
	prec localh = localMacroscopic[0];
	prec localux = localMacroscopic[1];
	prec localuy = localMacroscopic[2];
	prec gh  = 1.5 * 9.8 * localh;
	prec usq = 1.5 * (localux * localux + localuy * localuy);
	prec ux3 = 3.0 * e * localux;
	prec uy3 = 3.0 * e * localuy;
	prec uxuy5 = ux3 + uy3;
	prec uxuy6 = uy3 - ux3;

	feq[0] = localh * (1 - factor * (5.0 * gh + 4.0 * usq));
	feq[1] = localh * factor * (gh + ux3 + 4.5 * ux3*ux3 * factor - usq);
	feq[2] = localh * factor * (gh + uy3 + 4.5 * uy3*uy3 * factor - usq);
	feq[3] = localh * factor * (gh - ux3 + 4.5 * ux3*ux3 * factor - usq);
	feq[4] = localh * factor * (gh - uy3 + 4.5 * uy3*uy3 * factor - usq);
	feq[5] = localh * factor * 0.25 * (gh + uxuy5 + 4.5 * uxuy5*uxuy5 * factor - usq);
	feq[6] = localh * factor * 0.25 * (gh + uxuy6 + 4.5 * uxuy6*uxuy6 * factor - usq);
	feq[7] = localh * factor * 0.25 * (gh - uxuy5 + 4.5 * uxuy5*uxuy5 * factor - usq);
	feq[8] = localh * factor * 0.25 * (gh - uxuy6 + 4.5 * uxuy6*uxuy6 * factor - usq);
}

__global__ void First(const configStruct config, prec* localMacroscopic, prec* forcing, prec* localf, 
	const prec* __restrict__ b, const unsigned char* __restrict__ binary1, 
	const unsigned char* __restrict__ binary2, const prec* __restrict__ f1, 
	prec* f2, prec* h) {
	int i = threadIdx.x + blockIdx.x*blockDim.x;	
	if (i < config.Lx*config.Ly) {
		unsigned char b1 = binary1[i];
		unsigned char b2 = binary2[i];
		if(b1 != 0 || b2 != 0){
			int ex[8] = {1,0,-1,0,1,-1,-1,1};		
			int ey[8] = {0,1,0,-1,1,1,-1,-1};
			#if PDE == 1
				prec factor = 1 / (6 * config.e*config.e);
				prec localh = h[i];
				prec localb = b[i];
				for (int j = 0; j < 4; j++){
					int index = IDX(i, j, config.Lx, ex, ey);
					if (index > 0 && index < config.Lx*config.Ly) {
					forcing[8*i+j] = factor * 9.8 * (localh + h[index]) * (b[index] - localb);
					} else {
						forcing[8*i+j] = 0.0;
					}
				}
				for (int j = 4; j < 8; j++){
					int index = IDX(i, j, config.Lx, ex, ey);
					if (index > 0 && index < config.Lx*config.Ly) {
					forcing[8*i+j] = factor * 0.25 * 9.8 * (localh + h[index]) * (b[index] - localb);
					} else {
						forcing[8*i+j] = 0.0;
					}
				}
			#elif PDE == 5
				calculateForcingUser(forcing, h, b, config.e, i, config.Lx, ex, ey);
			#else 
				for (int j = 0; j < 8; j++)
					forcing[8*i+j] = 0;
			#endif


			localf[9*i] = f1[i]; 
			for (int j = 1; j < 9; j++){
				if(((b1>>(j-1)) & 1) & (~(b2>>(j-1)) & 1)) 
					localf[9*i+j] = f1[IDXcm(IDX(i, j-1, config.Lx, ex, ey), j, config.Lx, config.Ly)] + forcing[8*i+j-1];
				else if((~(b1>>(j-1)) & 1) & (~(b2>>(j-1)) & 1)) 
					localf[9*i+j] = f1[IDXcm(i, j, config.Lx, config.Ly)];
			}

			for (int j = 1; j < 9; j++)
				if((~(b1>>(j-1)) & 1) & ((b2>>(j-1)) & 1)) 
					#if BC1 == 1
						OBC(localf, f1, i, j, config.Lx, config.Ly);
					#elif BC1 == 2
						PBC(localf, f1, i, j, config.Lx, config.Ly, ex, ey);
					#elif BC1 == 3
						BBBC(localf, j);
					#elif BC1 == 4
						SBC(localf, j, b1, b2);
					#elif BC1 == 5
						UBC1(localf, f1, i, j, config.Lx, config.Ly, ex, ey, b1, b2);
					#elif BC1 == 6
						UBC2(localf, f1, i, j, config.Lx, config.Ly, ex, ey, b1, b2);
					#endif

			#if BC2 != 0
			for (int j = 1; j < 9; j++)
				if(((b1>>(j-1)) & 1) & ((b2>>(j-1)) & 1)) 
					#if BC2 == 1
						localf[9*i+j] = OBC(localf, f1, i, j, config.Lx, config.Ly);
					#elif BC2 == 2
						localf[9*i+j] = PBC(localf, f1, i, j, config.Lx, config.Ly, ex, ey);
					#elif BC2 == 3
						localf[9*i+j] = BBBC(localf, j);
					#elif BC2 == 4
						localf[9*i+j] = SBC(localf, j, b1, b2);
					#elif BC2 == 5
						localf[9*i+j] = BC1User(localf, f1, i, j, config.Lx, config.Ly, ex, ey, b1, b2);
					#elif BC2 == 6
						localf[9*i+j] = BC2User(localf, f1, i, j, config.Lx, config.Ly, ex, ey, b1, b2);
					#endif
			#endif
			
		}
	} 
} 

__global__ void Second(const configStruct config, prec* localMacroscopic, prec* forcing, prec* localf, 
	const prec* __restrict__ b, const unsigned char* __restrict__ binary1, 
	const unsigned char* __restrict__ binary2, const prec* __restrict__ f1, 
	prec* f2, prec* h) {
	int i = threadIdx.x + blockIdx.x*blockDim.x;	
	if (i < config.Lx*config.Ly) {
		unsigned char b1 = binary1[i];
		unsigned char b2 = binary2[i];
		if(b1 != 0 || b2 != 0){
			calculateMacroscopic(localMacroscopic, localf, config.e, i);
			h[i] = (prec)localMacroscopic[3*i];

		}
	}
}

__global__ void Third(const configStruct config, prec* localMacroscopic, prec* forcing, prec* localf, 
	const prec* __restrict__ b, const unsigned char* __restrict__ binary1, 
	const unsigned char* __restrict__ binary2, const prec* __restrict__ f1, 
	prec* f2, prec* h) {
	int i = threadIdx.x + blockIdx.x*blockDim.x;	
	if (i < config.Lx*config.Ly) {
		unsigned char b1 = binary1[i];
		unsigned char b2 = binary2[i];
		if(b1 != 0 || b2 != 0){
			prec localMacroscopicTmp[3];

			localMacroscopicTmp[0] = (prec)localMacroscopic[3*i];
			localMacroscopicTmp[1] = (prec)localMacroscopic[3*i+1];
			localMacroscopicTmp[2] = (prec)localMacroscopic[3*i+2];

			prec feq[9];
			#if PDE == 1
				calculateFeqSWE(feq, localMacroscopicTmp, config.e);
			#elif PDE == 2
				calculateFeqHE(feq, localMacroscopicTmp, config.e);
			#elif PDE == 3
				calculateFeqWE(feq, localMacroscopicTmp, config.e);
			#elif PDE == 4
				calculateFeqNSE(feq, localMacroscopicTmp, config.e);
			#elif PDE == 5
				calculateFeqUser(feq, localMacroscopicTmp, config.e);
			#endif
			
			for (int j = 0; j < 9; j++)
				f2[IDXcm(i, j, config.Lx, config.Ly)] = localf[9*i+j] - (localf[9*i+j] - feq[j]) / config.tau;
		}
	}
}

__device__ void OBC(prec* localf, const prec* __restrict__ f, int i, int j, int Lx, int Ly){
	localf[9*i+j] = f[IDXcm(i, j, Lx, Ly)];
}

__device__ int IDX(int i, int j, int Lx, int* ex, int* ey){
	return i + ex[j] + ey[j] * Lx;
}

__device__ int IDXcm(int i, int j, int Lx, int Ly){
	return i + j * Lx * Ly;
}

__device__ void calculateFeqHE(prec* feq, prec* localMacroscopic, prec e){	
	prec factor = 1.0 / 9;	
	prec localT = localMacroscopic[0];

	feq[0] = localT * factor * 4;
	feq[1] = localT * factor;
	feq[2] = localT * factor;
	feq[3] = localT * factor;
	feq[4] = localT * factor;
	feq[5] = localT * factor * 0.25;
	feq[6] = localT * factor * 0.25;
	feq[7] = localT * factor * 0.25;
	feq[8] = localT * factor * 0.25;
}

