
/*-------------------------------------------------------------------------
 *
 * MATLAB MEX gateway for backprojection
 *
 * This file gets the data from MATLAB, checks it for errors and then 
 * parses it to C and calls the relevant C/CUDA fucntions.
 *
 * CODE by       Ander Biguri
 *
---------------------------------------------------------------------------
---------------------------------------------------------------------------
Copyright (c) 2015, University of Bath and CERN- European Organization for 
Nuclear Research
All rights reserved.

Redistribution and use in source and binary forms, with or without 
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, 
this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, 
this list of conditions and the following disclaimer in the documentation 
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE 
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
 ---------------------------------------------------------------------------

Contact: tigre.toolbox@gmail.com
Codes  : https://github.com/CERN/TIGRE
--------------------------------------------------------------------------- 
 */



#include "tmwtypes.h"
#include "mex.h"
#include "matrix.h"
#include "voxel_backprojection.hpp"
#include "voxel_backprojection2.hpp"

#include <string.h>
#include "voxel_backprojection_parallel.hpp"

// #include <time.h>

// Added by RB for OpenMP and multi-GPU support , 4/15/2017
#include <omp.h>
#include <cuda_runtime_api.h> // To get number of GPUs
#include <cuda.h>  


/**
 * MEX gateway
 *
 * This function takes data from MATLAB and passes it to the MEX code.
 * It checks and casts the inputs and prepares teh outputs for MATLAB.
 *
 *
 */

void mexFunction(int  nlhs , mxArray *plhs[],
        int nrhs, mxArray const *prhs[]){
    
    //Check amount of inputs
    if (nrhs<3 ||nrhs>4) {
        mexErrMsgIdAndTxt("CBCT:MEX:Atb:InvalidInput", "Wrong number of inputs provided");
    }
    /*
     ** 4rd argument is matched or un matched.
     */
    bool krylov_proj=false; // Caled krylov, because I designed it for krylov case....
    if (nrhs==4){
        if ( mxIsChar(prhs[3]) != 1)
            mexErrMsgIdAndTxt( "CBCT:MEX:Atb:InvalidInput","4rd input shoudl be a string");
        
        /* copy the string data from prhs[0] into a C string input_ buf.    */
        char *krylov = mxArrayToString(prhs[3]);
        if (strcmp(krylov,"FDK") && strcmp(krylov,"matched"))
            mexErrMsgIdAndTxt( "CBCT:MEX:Atb:InvalidInput","4rd input shoudl be either 'FDK' or 'matched'");
        else
            if (!strcmp(krylov,"matched"))
                krylov_proj=true;
    }
    /*
     ** Third argument: angle of projection.
     */
    size_t mrows,ncols;
    
    mrows = mxGetM(prhs[2]);
    ncols = mxGetN(prhs[2]);
    
    if( !mxIsDouble(prhs[2]) || mxIsComplex(prhs[2]) ||
            !(mrows==1) ) {
        mexErrMsgIdAndTxt( "CBCT:MEX:Atb:input",
                "Input alpha must be a double, noncomplex array.");
    }
    
    size_t nalpha=ncols;
    mxArray const * const ptralphas=prhs[2];
    
    double const * const alphasM= static_cast<double const *>(mxGetData(ptralphas));
    // just copy paste the data to a float array
    float  *  alphas= (float*)malloc(nalpha*sizeof(float));
    for (int i=0;i<nalpha;i++)
        alphas[i]=(float)alphasM[i];
    
    /**
     * First input: The projections
     */
    
    // First input should be b from (Ax=b) i.e. the projections.
    mxArray const * const image = prhs[0];                 // Get pointer of the data
    mwSize const numDims = mxGetNumberOfDimensions(image); // Get numer of Dimensions of input matrix.
    // Image should be dim 3
    if (!(numDims==3 && nalpha>1) && !(numDims==2 && nalpha==1) ){
        mexErrMsgIdAndTxt("CBCT:MEX:Atb:InvalidInput",  "Projection data is not the rigth size");
    }
     if( !mxIsSingle(prhs[0])) {
       mexErrMsgIdAndTxt("CBCT:MEX:Ax:InvalidInput",
                "Input image must be a single noncomplex array.");
     }
    // Now that input is ok, parse it to C data types.
    // NOTE: while Number of dimensions is the size of the matrix in Matlab, the data is 1D row-wise mayor.
    
    // We need a float image, and, unfortunatedly, the only way of casting it is by value
    const mwSize *size_proj= mxGetDimensions(image); //get size of image
    mrows = mxGetM(image);
    ncols = mxGetN(image);
    size_t size_proj2;
    if (nalpha==1)
        size_proj2=1;
    else
        size_proj2=size_proj[2];
    
    
    // float const * const imgaux = static_cast<float const *>(mxGetData(image));
	float const * const img = static_cast<float const *>(mxGetData(image));

    // MODIFICATION, RB, 5/23/2017: no need to permute (throughput fix from Ander), just make imgaux above img.
    
 //   float *  img = (float*)malloc(size_proj[0] *size_proj[1] *size_proj2* sizeof(float));


 //   const long size0 = size_proj[0];
 //   const long size1 = size_proj[1];
 //   const long size2 = size_proj2;
 //   // Permute(imgaux,[2 1 3]);
 //   
	//// ADDED by RB, 4/15/2017 to speed things up (parallelizing the outer loop below actually speeds up this reshaping operation significantly)
	//int numThreads = omp_get_num_procs() / 2;  // Let's not use all logical processors, just half should be enough (with too many threads we are becoming memory bound anyway, I believe)
	//// Sanity check for num threads (unlikely to happen on modern multi-core processors, but just to be safe)
	//if (numThreads < 2) numThreads = 2;

	//// numThreads = 1;  // For testing only

	//long long j;
	//#pragma omp parallel for default(shared) private(j) num_threads(numThreads) schedule(guided)
	//// for (unsigned int j = 0; j < size2; j++)
	//for (j = 0; j < size2; j++)
	//{
	//	long long jOffset = j*size0*size1;
	//	for (long long k = 0; k < size0; k++)
	//	{
	//		long long kOffset1 = k*size1;
	//		for (long long i = 0; i < size1; i++)
	//		{
	//			long long iOffset2 = i*size0;
	//			img[i + jOffset + kOffset1] = imgaux[iOffset2 + jOffset + k];
	//		}
	//	}
	//}

    
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /**
     * Second input: Geometry structure
     */
    mxArray * geometryMex=(mxArray*)prhs[1];
    
    // IMPORTANT-> Make sure Matlab creates the struct in this order.
    const char *fieldnames[14];
    fieldnames[0] = "nVoxel";
    fieldnames[1] = "sVoxel";
    fieldnames[2] = "dVoxel";
    fieldnames[3] = "nDetector";
    fieldnames[4] = "sDetector";
    fieldnames[5] = "dDetector";
    fieldnames[6] = "DSD";
    fieldnames[7] = "DSO";
    fieldnames[8] = "offOrigin";
    fieldnames[9] = "offDetector";
    fieldnames[10]= "accuracy";
    fieldnames[11]= "mode";
    fieldnames[12]= "COR";
    fieldnames[13]= "rotDetector";
    // Make sure input is structure
    if(!mxIsStruct(geometryMex))
        mexErrMsgIdAndTxt( "CBCT:MEX:Atb:InvalidInput",
                "Second input must be a structure.");
    // Check number of fields
    int nfields = mxGetNumberOfFields(geometryMex);
    if (nfields < 10 || nfields >14 )
        mexErrMsgIdAndTxt("CBCT:MEX:Atb:InvalidInput","There are missing or extra fields in the geometry");
    
    mxArray    *tmp;
    bool offsetAllOrig=false;
    bool offsetAllDetec=false;
    bool rotAllDetec=false;
    bool CORAll=false;
    for(int ifield=0; ifield<14; ifield++) {
        tmp=mxGetField(geometryMex,0,fieldnames[ifield]);
        if(tmp==NULL){
           //tofix
            continue;
        }
        switch(ifield){
            
            // cases where we want 3 input arrays.
            case 0:case 1:case 2:
                mrows = mxGetM(tmp);
                ncols = mxGetN(tmp);
                if (mrows!=3 || ncols!=1){
                    mexPrintf("%s %s \n", "FIELD: ", fieldnames[ifield]);
                    mexErrMsgIdAndTxt( "CBCT:MEX:Atb:InvalidInput",
                            "Above field has wrong size! Should be 3x1!");
                }
                break;
                // this ones should be 2x1
            case 3:case 4:case 5:
                mrows = mxGetM(tmp);
                ncols = mxGetN(tmp);
                if (mrows!=2 || ncols!=1){
                    mexPrintf("%s %s \n", "FIELD: ", fieldnames[ifield]);
                    mexErrMsgIdAndTxt( "CBCT:MEX:Atb:InvalidInput",
                            "Above field has wrong size! Should be 2x1!");
                }
                break;
                // this ones should be 1x1
                
            case 12://COR
                mrows = mxGetM(tmp);
                ncols = mxGetN(tmp);

                if (mrows!=1 || ( ncols!=1&& ncols!=nalpha) ){
                    mexPrintf("%s %s \n", "FIELD: ", fieldnames[ifield]);
                    mexPrintf("%ld x %ld \n", "FIELD: ", (long int)mrows,(long int)ncols);
                    mexErrMsgIdAndTxt( "CBCT:MEX:Ax:inputsize",
                            "Above field has wrong size! Should be 3x1 or 3xlength(angles)!");
                    
                }
               
                if (ncols==nalpha)
                    CORAll=true;
                break;
                
            case 6:case 7:case 10:
                mrows = mxGetM(tmp);
                ncols = mxGetN(tmp);
                if (mrows!=1 || ncols!=1){
                    mexPrintf("%s %s \n", "FIELD: ", fieldnames[ifield]);
                    mexErrMsgIdAndTxt("CBCT:MEX:Atb:InvalidInput",
                            "Above field has wrong size! Should be 1x1!");
                }
                break;
            case 8:
                mrows = mxGetM(tmp);
                ncols = mxGetN(tmp);
                if (mrows!=3 || ( ncols!=1&& ncols!=nalpha) ){
                    mexPrintf("%s %s \n", "FIELD: ", fieldnames[ifield]);
                    mexErrMsgIdAndTxt( "CBCT:MEX:Ax:inputsize",
                            "Above field has wrong size! Should be 3x1 or 3xlength(angles)!");
                    
                }
                
                if (ncols==nalpha)
                    offsetAllOrig=true;
                
                break;
                
            case 9:
                mrows = mxGetM(tmp);
                ncols = mxGetN(tmp);
                if (mrows!=2 || ( ncols!=1&& ncols!=nalpha)){
                    mexPrintf("%s %s \n", "FIELD: ", fieldnames[ifield]);
                    mexErrMsgIdAndTxt( "CBCT:MEX:Ax:inputsize",
                            "Above field has wrong size! Should be 2x1 or 3xlength(angles)!");
                    
                }
                
                if (ncols==nalpha)
                    offsetAllDetec=true;
                
                break;
            case 11:
                if (!mxIsChar(tmp)){
                    mexPrintf("%s %s \n", "FIELD: ", fieldnames[ifield]);
                    mexErrMsgIdAndTxt( "CBCT:MEX:Ax:inputsize",
                            "Above field is not string!");
                }
                
                break;
            case 13:
                mrows = mxGetM(tmp);
                ncols = mxGetN(tmp);
                if (mrows!=3 || ( ncols!=1&& ncols!=nalpha)){
                    mexPrintf("%s %s \n", "FIELD: ", fieldnames[ifield]);
                    mexErrMsgIdAndTxt( "CBCT:MEX:Ax:inputsize",
                            "Above field has wrong size! Should be 3x1 or 3xlength(angles)!");
                   
                }
                
                if (ncols==nalpha)
                    rotAllDetec=true;
                break;
            default:
                mexErrMsgIdAndTxt( "CBCT:MEX:Atb:InvalidInput",
                        "Something wrong happened. Ensure Geometric struct has correct amount of inputs.");
        }
        
    }
    // Now we know that all the input struct is good! Parse it from mxArrays to
    // C structures that MEX can understand.
    
    double * nVoxel, *nDetec; //we need to cast these to int
    double * sVoxel, *dVoxel,*sDetec,*dDetec, *DSO, *DSD,*offOrig,*offDetec;
    double *acc, *COR,*rotDetector;
    const char* mode;
    bool coneBeam=true;
    Geometry geo;

	// ADDITION, RB, 4/18/2017: Initialize all pointers in geo to NULL to allow deleting allocated memory
	// at the end of this function. Otherwise we'll be leaking memory. This would be best handled if geo was a C++ class 
	// with a destructor, but since it's a struct, I have to do it this way.
	geo.offOrigX = geo.offOrigY = geo.offOrigZ = geo.offDetecU = geo.offDetecV = geo.dRoll = geo.dPitch = geo.dYaw = geo.COR = NULL;

    int c;
    geo.unitX=1;geo.unitY=1;geo.unitZ=1;
    for(int ifield=0; ifield<14; ifield++) {
        tmp=mxGetField(geometryMex,0,fieldnames[ifield]);
          if(tmp==NULL){
           //tofix
            continue;
        }
        switch(ifield){
            case 0:
                nVoxel=(double *)mxGetData(tmp);
                // copy data to MEX memory
                geo.nVoxelX=(int)nVoxel[0];
                geo.nVoxelY=(int)nVoxel[1];
                geo.nVoxelZ=(int)nVoxel[2];
                break;
            case 1:
                sVoxel=(double *)mxGetData(tmp);
                geo.sVoxelX=(float)sVoxel[0];
                geo.sVoxelY=(float)sVoxel[1];
                geo.sVoxelZ=(float)sVoxel[2];
                break;
            case 2:
                dVoxel=(double *)mxGetData(tmp);
                geo.dVoxelX=(float)dVoxel[0];
                geo.dVoxelY=(float)dVoxel[1];
                geo.dVoxelZ=(float)dVoxel[2];
                break;
            case 3:
                nDetec=(double *)mxGetData(tmp);
                geo.nDetecU=(int)nDetec[0];  
                geo.nDetecV=(int)nDetec[1];
                break;
            case 4:
                sDetec=(double *)mxGetData(tmp);
                geo.sDetecU=(float)sDetec[0];  
                geo.sDetecV=(float)sDetec[1];
                break;
            case 5:
                dDetec=(double *)mxGetData(tmp);
                geo.dDetecU=(float)dDetec[0];  
                geo.dDetecV=(float)dDetec[1];
                break;
            case 6:
                DSD=(double *)mxGetData(tmp);
                geo.DSD=(float)DSD[0];
                break;
            case 7:
                DSO=(double *)mxGetData(tmp);
                geo.DSO=(float)DSO[0];
            case 8:
                
                geo.offOrigX=(float*)malloc(nalpha * sizeof(float));
                geo.offOrigY=(float*)malloc(nalpha * sizeof(float));
                geo.offOrigZ=(float*)malloc(nalpha * sizeof(float));
                
                offOrig=(double *)mxGetData(tmp);
                
                for (int i=0;i<nalpha;i++){
                    if (offsetAllOrig)
                        c=i;
                    else
                        c=0;
                    geo.offOrigX[i]=(float)offOrig[0+3*c];
                    geo.offOrigY[i]=(float)offOrig[1+3*c];
                    geo.offOrigZ[i]=(float)offOrig[2+3*c];
                }
                break;
            case 9:
                geo.offDetecU=(float*)malloc(nalpha * sizeof(float));
                geo.offDetecV=(float*)malloc(nalpha * sizeof(float));
                
                offDetec=(double *)mxGetData(tmp);
                for (int i=0;i<nalpha;i++){
                    if (offsetAllDetec)
                        c=i;
                    else
                        c=0;
                    geo.offDetecU[i]=(float)offDetec[0+2*c];
                    geo.offDetecV[i]=(float)offDetec[1+2*c];
                }
                break;
            case 10:
                acc=(double*)mxGetData(tmp);
                geo.accuracy=(float)acc[0];
                break;
            case 11:
                mode="";
                mode=mxArrayToString(tmp);
                if (!strcmp(mode,"parallel"))
                    coneBeam=false;
                else if (strcmp(mode,"cone"))
                    mexErrMsgIdAndTxt( "CBCT:MEX:Atb:Mode","Unkown mode. Should be parallel or cone");
                break;
             case 12:
                COR=(double*)mxGetData(tmp);
                geo.COR=(float*)malloc(nalpha * sizeof(float));
                 for (int i=0;i<nalpha;i++){
                    if (CORAll)
                        c=i;
                    else
                        c=0;
                    
                    geo.COR[i]  = (float)COR[0+c]; 
                }
                break;
            case 13:
                geo.dRoll= (float*)malloc(nalpha * sizeof(float));
                geo.dPitch=(float*)malloc(nalpha * sizeof(float));
                geo.dYaw=  (float*)malloc(nalpha * sizeof(float));
                
                rotDetector=(double *)mxGetData(tmp);
                
                for (int i=0;i<nalpha;i++){
                    if (rotAllDetec)
                        c=i;
                    else
                        c=0;
                    
                    geo.dYaw[i]  = (float)rotDetector[0+3*c];
                    geo.dPitch[i]= (float)rotDetector[1+3*c];
                    geo.dRoll[i] = (float)rotDetector[2+3*c];

                }
                break;
            default:
                mexErrMsgIdAndTxt( "CBCT:MEX:Atb:unknown","This shoudl not happen. Weird");
                break;
                
        }
    }
    
    //Spetiall cases
    // Accuracy
    tmp=mxGetField(geometryMex,0,fieldnames[10]);
    if (tmp==NULL)
        geo.accuracy=0.5;
    // Geometry
    tmp=mxGetField(geometryMex,0,fieldnames[11]);
    if (tmp==NULL)
        coneBeam=true;
   // COR
    tmp=mxGetField(geometryMex,0,fieldnames[12]);
    if (tmp==NULL){
        geo.COR=(float*)malloc(nalpha * sizeof(float));
        memset(geo.COR,0,nalpha * sizeof(float));
    }
    // angle rotation detector
    tmp=mxGetField(geometryMex,0,fieldnames[13]);
    if (tmp==NULL){
       
        geo.dRoll= (float*)malloc(nalpha * sizeof(float));
        geo.dPitch=(float*)malloc(nalpha * sizeof(float));
        geo.dYaw=  (float*)malloc(nalpha * sizeof(float));
        memset(geo.dRoll,0,nalpha * sizeof(float));
        memset(geo.dPitch,0,nalpha * sizeof(float));
        memset(geo.dYaw,0,nalpha * sizeof(float));
    }
    /*
     * allocate memory for the output
     */
    
	 /*
	 * Prepare the outputs
	 */

	 // MODIFICATTION, RB, 5/12/2017: get the pointer to the output here, so that we can use it directly and minimize
	 // the need for auxiliary buffers and copying around the data.
	mwSize imgsize[3];
	imgsize[0] = geo.nVoxelX;
	imgsize[1] = geo.nVoxelY;
	imgsize[2] = geo.nVoxelZ;

	plhs[0] = mxCreateNumericArray(3, imgsize, mxSINGLE_CLASS, mxREAL);
	float *outImage = (float *)mxGetPr(plhs[0]);

	// ADDED by RB, 4/15/2017 to support multiple GPUs (via OpenMP)
	int numGPUsAux = 0;       // number of CUDA GPUs
	cudaGetDeviceCount(&numGPUsAux);

	// ADDDITION, RB: This is a temporary prototype solution designed for our specific systems using Quadro P5000 cards. This should
	// be eventually substituted with a more flexible and general solution. Here we check how many Quadro P5000 cards we have installed
	// and select only their IDs to be used in the multi-GPU computation. The idea is that we want to select the cards of the same type/RAM amount and ignore
	// any "weaker" GPUs that may be used for display only, as is the case on the MRD system.
	int* validGPUIndexes = (int*)malloc(numGPUsAux * sizeof(int));

	int numGPUs = 0;  // We will count the number of P5000 cards with this variable

	for (int i = 0; i < numGPUsAux; i++)
	{
		cudaDeviceProp prop;
		cudaGetDeviceProperties(&prop, i);
		if (strstr(prop.name, "P5000")!=NULL)  // Means we found substring "P5000" in the device name
		{
			validGPUIndexes[numGPUs] = i;  // Remember device index of this P5000 card (indexes may not be consecutive!)
			numGPUs++;   // Increment number of P5000 devices found
		}  // END if P5000 found

	}  // END for iterating through all GPUs

	// numGPUs = 1;  // For testing only!

	// We have to replicate the output volume for each GPU (then we'll combine the results from multiple GPUS by summation)
	// We'll keep pointers to the results in an array.
	float** resTable = (float**)malloc(numGPUs * sizeof(float*));

	// Allocate the output volumes
	// IMPORTANT!!!! Note that we only allocate (malloc) buffers from volNo=1, since the buffer number
	// 0 is set to outImage. This saves us memory and one unnecessary copy of data
	resTable[0] = outImage;  // This will be our final output (just use MEX variable, do not allocate an extra copy here)
	unsigned long long numBytesInVolume = (unsigned long long)geo.nVoxelX *geo.nVoxelY*geo.nVoxelZ * sizeof(float);
	for (int volNo = 1; volNo < numGPUs; volNo++)  // starts from 1!!!!
		resTable[volNo] = (float*)malloc(numBytesInVolume);
    
    /*
     * Call the CUDA kernel
     */
    if (coneBeam){
        if (krylov_proj)
		{
         //   voxel_backprojection2(img,geo,result,alphas,nalpha);
        }
        else
		{
			int i;
			int noOfProjInGroup = nalpha / numGPUs;

			// Check if the number of projections is divisible by number of GPUs. If not, we need to use a different number
			// of projections in group for the LAST projection group only (last iteration of the loop below)
			int projRemainder = nalpha % numGPUs;

			#pragma omp parallel for default(shared) private(i) num_threads(numGPUs) schedule(guided)
			for (i = 0; i < numGPUs; i++)
			{
				// Offset to the index of the first angle in the projection group
				int projectionIndexOffset = i*noOfProjInGroup; // Will be 0 for the first group
															   // Offset to the index of the first projection DATA in the projection  group
				long long projDataOffset = (long long)projectionIndexOffset*geo.nDetecU *geo.nDetecV;  // number of first projection in group times projection size
				// Now we may need to use a different projection group size for the LAST projection group if the 
				// number of projections is not divisible by the number of GPUs (in other words we have a remainder from division)
				int currNoOfProjInGroup = noOfProjInGroup;  // Will be true for all groups except perhaps the last (if we have non-zero reminder)
				if (i == (numGPUs - 1) && projRemainder != 0)  // can only happen for the last group
					currNoOfProjInGroup = nalpha - (numGPUs - 1)*noOfProjInGroup;  // Gives us the number different from noOfProjInGroup

				// voxel_backprojection(&img[projDataOffset], geo, resTable[i], alphas, projectionIndexOffset, currNoOfProjInGroup, i);  // i defines the ID of GPU to use
				voxel_backprojection(&img[projDataOffset], geo, resTable[i], alphas, projectionIndexOffset, currNoOfProjInGroup, validGPUIndexes[i]);  // we get indexes of our P5000 cards from validGPUIndexes

			}  // END parallel for 
		}  // END if !krylov_proj
    }  //  END if cone beam
	else  // If not cone beam
	{
//         if (krylov_proj){
//             voxel_backprojection_parallel2(img,geo,result,alphas,nalpha);
//             
//         }
//         else{
       //     voxel_backprojection_parallel(img,geo,result,alphas,nalpha);
//         }

    }
    
	// ADDED, RB, 4/18/2017: We need to reset ALL GPUs here, otherwise it leaves them in a state that later causes MATLAB to crash. 
	// This happens ONLY if more than one GPU are run IN PARALLEL (was not observed for sequential operation of two GPUs).
	// I do not know what exactly gets messed up during parallel execution on more than one GPUs, but MATLAB may then crash when
	// any attempt to access a GPU (e.g. by calling "gpuDevice()") is made. Resetting the GPUs below fixes the problem (however, the computation
	// itself was always successfully completed, the crash happened on any subsequent attempt to use a GPU in MATLAB).
	// Actually, here let's make sure we reset ALL GPUs, regardless of what the numGPUs was set to be (e.g. if we really have 2 GPUs, but wanted to test only with one;
	// in such a case I unfortunately got MATLAB crashes as well even if I only reset ONE of the two GPUs below even though I only used one GPU in my test)
	int actualNumGPUs = 0;       // number of CUDA GPUs
	cudaGetDeviceCount(&actualNumGPUs);

	for (int i = 0; i < actualNumGPUs; i++)
	{
		cudaSetDevice(i);
		cudaDeviceReset();
	}  // END for


	long long i;

	// ADDED by RB, 4/15/2017 to speed things up (parallelizing the outer loop below actually speeds things up)
	int numThreads = omp_get_num_procs() / 2;  // Let's not use all logical processors, just half should be enough (with too many threads we are becoming memory bound anyway, I believe)
	// Sanity check for num threads (unlikely to happen on modern multi-core processors, but just to be safe)
	if (numThreads < 2) numThreads = 2;

	// numThreads = 1;  // For testing only

	// Combine the results from our replicated auxiliary volumes
	// ADDED by RB, 4/15/2017: Parallelize the loop to speed things up!
	#pragma omp parallel for default(shared) private(i) num_threads(numThreads) schedule(guided)
	for (i = 0; i < geo.nVoxelX*geo.nVoxelY*geo.nVoxelZ; i++)
	{
		// IMPORTANT!!!! Note that we start from volNo=1 below and not from 0, since resTable[0] is actually outImage. 
		// This is not an error. We just want to add the remaining auxiliary volume copies to outImage, which was filled directly
		// by call to voxel_backprojection() above
		for (int volNo = 1; volNo < numGPUs; volNo++)
			outImage[i] += resTable[volNo][i];

	}  // END for (outer parallel loop)

	   // Deallocate the auxiliary output volumes
	   // IMPORTANT!!!! Note that we start from volNo=1 and not 0 below. This is not an error, since resTable[0] is set to outImage and we do 
	   // NOT want to deallocate outImage storage (outImage is returned to MATLAB)!!!!
	for (int volNo = 1; volNo < numGPUs; volNo++)
		free(resTable[volNo]);

	free(resTable);

	free(validGPUIndexes);   // TEMPORARY (until a more general solution is implemented)

	// COMMENTED OUT 5/23/2017, RB: We no longer need to clear img memory, since img is no longer a copy of the input data:
	// it is a pointer to MEX data. Actually if we free it, we'll blow something up.
	// free(img);

	// ADDDITION (bug fix), RB, 5/9/2017: Also deallocate alphas (otherwise we leak memory)
	free(alphas);

	// ADDITION (bug fix), RB, 4/18/2017: Deallocate memory allocated in geo (only if a pointer was allocated, i.e. it is not NULL)
	if (geo.offOrigX != NULL) free(geo.offOrigX);
	if (geo.offOrigY != NULL) free(geo.offOrigY);
	if (geo.offOrigZ != NULL) free(geo.offOrigZ);
	if (geo.offDetecU != NULL) free(geo.offDetecU);
	if (geo.offDetecV != NULL)  free(geo.offDetecV);
	if (geo.dRoll != NULL) free(geo.dRoll);
	if (geo.dPitch != NULL) free(geo.dPitch);
	if (geo.dYaw != NULL) free(geo.dYaw);
	if (geo.COR != NULL) free(geo.COR);
	
	return;
}


