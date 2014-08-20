/**
 * Record the force on an atom to global memory.
 */
inline __device__ void storeForce(int atom, real3 force, unsigned long long* __restrict__ forceBuffers) {
    atomicAdd(&forceBuffers[atom], static_cast<unsigned long long>((long long) (force.x*0x100000000)));
    atomicAdd(&forceBuffers[atom+PADDED_NUM_ATOMS], static_cast<unsigned long long>((long long) (force.y*0x100000000)));
    atomicAdd(&forceBuffers[atom+2*PADDED_NUM_ATOMS], static_cast<unsigned long long>((long long) (force.z*0x100000000)));
}

/**
 * Convert a real4 to a real3 by removing its last element.
 */
inline __device__ real3 trim(real4 v) {
    return make_real3(v.x, v.y, v.z);
}

/**
 * Compute the difference between two vectors, taking periodic boundary conditions into account
 * and setting the fourth component to the squared magnitude.
 */
inline __device__ real4 delta(real4 vec1, real4 vec2, real4 periodicBoxSize, real4 invPeriodicBoxSize) {
    real4 result = make_real4(vec1.x-vec2.x, vec1.y-vec2.y, vec1.z-vec2.z, 0.0f);
#ifdef USE_PERIODIC
    result.x -= floor(result.x*invPeriodicBoxSize.x+0.5f)*periodicBoxSize.x;
    result.y -= floor(result.y*invPeriodicBoxSize.y+0.5f)*periodicBoxSize.y;
    result.z -= floor(result.z*invPeriodicBoxSize.z+0.5f)*periodicBoxSize.z;
#endif
    result.w = result.x*result.x + result.y*result.y + result.z*result.z;
    return result;
}

/**
 * Compute the angle between two vectors.  The w component of each vector should contain the squared magnitude.
 */
__device__ real computeAngle(real4 vec1, real4 vec2) {
    real dotProduct = vec1.x*vec2.x + vec1.y*vec2.y + vec1.z*vec2.z;
    real cosine = dotProduct*RSQRT(vec1.w*vec2.w);
    real angle;
    if (cosine > 0.99f || cosine < -0.99f) {
        // We're close to the singularity in acos(), so take the cross product and use asin() instead.

        real3 crossProduct = cross(vec1, vec2);
        real scale = vec1.w*vec2.w;
        angle = ASIN(SQRT(dot(crossProduct, crossProduct)/scale));
        if (cosine < 0.0f)
            angle = M_PI-angle;
    }
    else
       angle = ACOS(cosine);
    return angle;
}

/**
 * Compute the cross product of two vectors, setting the fourth component to the squared magnitude.
 */
inline __device__ real4 computeCross(real4 vec1, real4 vec2) {
    real3 cp = cross(vec1, vec2);
    return make_real4(cp.x, cp.y, cp.z, cp.x*cp.x+cp.y*cp.y+cp.z*cp.z);
}

/**
 * Determine whether a particular interaction is in the list of exclusions.
 */
inline __device__ bool isInteractionExcluded(int atom1, int atom2, int* __restrict__ exclusions, int* __restrict__ exclusionStartIndex) {
    int first = exclusionStartIndex[atom1];
    int last = exclusionStartIndex[atom1+1];
    for (int i = last-1; i >= first; i--) {
        int excluded = exclusions[i];
        if (excluded == atom2)
            return true;
        if (excluded <= atom1)
            return false;
    }
    return false;
}

#define WARP_SIZE 32

/**
 * Perform a parallel prefix sum of boolean values over an array.  This is done as the first stage of compacting an array.
 */
__device__ void prefixSum(bool value, short* sum, ushort2* temp) {
#if __CUDA_ARCH__ >= 300
    const int indexInWarp = threadIdx.x%WARP_SIZE;
    const int warpMask = (2<<indexInWarp)-1;
    temp[threadIdx.x].x = __popc(__ballot(value)&warpMask);
    __syncthreads();
    if (threadIdx.x < WARP_SIZE) {
        int multiWarpSum = temp[(threadIdx.x+1)*WARP_SIZE-1].x;
        for (int offset = 1; offset < blockDim.x/WARP_SIZE; offset *= 2) {
            short n = __shfl_up(multiWarpSum, offset, WARP_SIZE);
            if (indexInWarp >= offset)
                multiWarpSum += n;
        }
        temp[threadIdx.x].y = multiWarpSum;
    }
    __syncthreads();
    sum[threadIdx.x] = temp[threadIdx.x].x+(threadIdx.x < WARP_SIZE ? 0 : temp[threadIdx.x/WARP_SIZE-1].y);
    __syncthreads();
#else
    temp[threadIdx.x].x = value;
    __syncthreads();
    int whichBuffer = 0;
    for (int offset = 1; offset < blockDim.x; offset *= 2) {
        if (whichBuffer == 0)
            temp[threadIdx.x].y = (threadIdx.x < offset ? temp[threadIdx.x].x : temp[threadIdx.x].x+temp[threadIdx.x-offset].x);
        else
            temp[threadIdx.x].x = (threadIdx.x < offset ? temp[threadIdx.x].y : temp[threadIdx.x].y+temp[threadIdx.x-offset].y);
        whichBuffer = 1-whichBuffer;
        __syncthreads();
    }
    if (whichBuffer == 0)
        sum[threadIdx.x] = temp[threadIdx.x].x;
    else
        sum[threadIdx.x] = temp[threadIdx.x].y;
    __syncthreads();
#endif
}

/**
 * Compute the interaction.
 */
extern "C" __global__ void computeInteraction(
        unsigned long long* __restrict__ forceBuffers, real* __restrict__ energyBuffer, const real4* __restrict__ posq,
        real4 periodicBoxSize, real4 invPeriodicBoxSize
#ifdef USE_FILTERS
        , int* __restrict__ particleTypes, int* __restrict__ orderIndex, int* __restrict__ particleOrder
#endif
#ifdef USE_EXCLUSIONS
        , int* __restrict__ exclusions, int* __restrict__ exclusionStartIndex
#endif
        PARAMETER_ARGUMENTS) {
    real energy = 0.0f;
    
    // Loop over particles to be the first one in the set.
    
    for (int p1 = blockIdx.x; p1 < NUM_ATOMS; p1 += gridDim.x) {
        int numNeighbors = NUM_ATOMS-p1-1;
        int numCombinations = NUM_CANDIDATE_COMBINATIONS;
        for (int index = threadIdx.x; index < numCombinations; index += blockDim.x) {
            FIND_ATOMS_FOR_COMBINATION_INDEX;
            bool includeInteraction = IS_VALID_COMBINATION;
#ifdef USE_CUTOFF
            if (includeInteraction) {
                VERIFY_CUTOFF;
            }
#endif
#ifdef USE_FILTERS
            int order = orderIndex[COMPUTE_TYPE_INDEX];
            if (order == -1)
                includeInteraction = false;
#endif
#ifdef USE_EXCLUSIONS
            if (includeInteraction) {
                VERIFY_EXCLUSIONS;
            }
#endif
            if (includeInteraction) {
                PERMUTE_ATOMS;
                LOAD_PARTICLE_DATA;
                COMPUTE_INTERACTION;
            }
        }
    }
    energyBuffer[blockIdx.x*blockDim.x+threadIdx.x] += energy;
}

/**
 * Find a bounding box for the atoms in each block.
 */
extern "C" __global__ void findBlockBounds(real4 periodicBoxSize, real4 invPeriodicBoxSize, const real4* __restrict__ posq,
        real4* __restrict__ blockCenter, real4* __restrict__ blockBoundingBox, int* __restrict__ numNeighborPairs) {
    int index = blockIdx.x*blockDim.x+threadIdx.x;
    int base = index*TILE_SIZE;
    while (base < NUM_ATOMS) {
        real4 pos = posq[base];
#ifdef USE_PERIODIC
        pos.x -= floor(pos.x*invPeriodicBoxSize.x)*periodicBoxSize.x;
        pos.y -= floor(pos.y*invPeriodicBoxSize.y)*periodicBoxSize.y;
        pos.z -= floor(pos.z*invPeriodicBoxSize.z)*periodicBoxSize.z;
#endif
        real4 minPos = pos;
        real4 maxPos = pos;
        int last = min(base+TILE_SIZE, NUM_ATOMS);
        for (int i = base+1; i < last; i++) {
            pos = posq[i];
#ifdef USE_PERIODIC
            real4 center = 0.5f*(maxPos+minPos);
            pos.x -= floor((pos.x-center.x)*invPeriodicBoxSize.x+0.5f)*periodicBoxSize.x;
            pos.y -= floor((pos.y-center.y)*invPeriodicBoxSize.y+0.5f)*periodicBoxSize.y;
            pos.z -= floor((pos.z-center.z)*invPeriodicBoxSize.z+0.5f)*periodicBoxSize.z;
#endif
            minPos = make_real4(min(minPos.x,pos.x), min(minPos.y,pos.y), min(minPos.z,pos.z), 0);
            maxPos = make_real4(max(maxPos.x,pos.x), max(maxPos.y,pos.y), max(maxPos.z,pos.z), 0);
        }
        real4 blockSize = 0.5f*(maxPos-minPos);
        blockBoundingBox[index] = blockSize;
        blockCenter[index] = 0.5f*(maxPos+minPos);
        index += blockDim.x*gridDim.x;
        base = index*TILE_SIZE;
    }
    if (blockIdx.x == 0 && threadIdx.x == 0)
        *numNeighborPairs = 0;
}

/**
 * Find a list of neighbors for each atom.
 */
extern "C" __global__ void findNeighbors(real4 periodicBoxSize, real4 invPeriodicBoxSize, const real4* __restrict__ posq,
        const real4* __restrict__ blockCenter, const real4* __restrict__ blockBoundingBox, int2* __restrict__ neighborPairs,
        int* __restrict__ numNeighborPairs, int* __restrict__ numNeighborsForAtom, int maxNeighborPairs
#ifdef USE_EXCLUSIONS
        , int* __restrict__ exclusions, int* __restrict__ exclusionStartIndex
#endif
        ) {
    for (int atom1 = blockIdx.x*blockDim.x+threadIdx.x; atom1 < NUM_ATOMS; atom1 += blockDim.x*gridDim.x) {
        // Load data for this atom.
        
        real4 pos1 = posq[atom1];
        int block1 = atom1/TILE_SIZE;
        real4 blockCenter1 = blockCenter[block1];
        real4 blockSize1 = blockBoundingBox[block1];
        int totalNeighborsForAtom1 = 0;
        
        // Loop over atom blocks to search for neighbors.
        
        for (int block2 = block1; block2 < NUM_BLOCKS; block2++) {
            real4 blockCenter2 = blockCenter[block2];
            real4 blockSize2 = blockBoundingBox[block2];
            real4 blockDelta = blockCenter1-blockCenter2;
#ifdef USE_PERIODIC
            blockDelta.x -= floor(blockDelta.x*invPeriodicBoxSize.x+0.5f)*periodicBoxSize.x;
            blockDelta.y -= floor(blockDelta.y*invPeriodicBoxSize.y+0.5f)*periodicBoxSize.y;
            blockDelta.z -= floor(blockDelta.z*invPeriodicBoxSize.z+0.5f)*periodicBoxSize.z;
#endif
            blockDelta.x = max(0.0f, fabs(blockDelta.x)-blockSize1.x-blockSize2.x);
            blockDelta.y = max(0.0f, fabs(blockDelta.y)-blockSize1.y-blockSize2.y);
            blockDelta.z = max(0.0f, fabs(blockDelta.z)-blockSize1.z-blockSize2.z);
            if (blockDelta.x*blockDelta.x+blockDelta.y*blockDelta.y+blockDelta.z*blockDelta.z < CUTOFF_SQUARED) {
                // Loop over atoms in this block.
                
                int start = block2*TILE_SIZE;
                int end = (block2+1)*TILE_SIZE;
                int included[TILE_SIZE];
                int numIncluded = 0;
                for (int atom2 = start; atom2 < end; atom2++) {
                    real4 pos2 = posq[atom2];
                    
                    // Decide whether to include this atom pair in the neighbor list.
                    
                    real4 atomDelta = delta(pos1, pos2, periodicBoxSize, invPeriodicBoxSize);
                    bool includeAtom = (atom2 > atom1 && atom2 < NUM_ATOMS && atomDelta.w < CUTOFF_SQUARED);
#ifdef USE_EXCLUSIONS
                    if (includeAtom)
                        includeAtom &= !isInteractionExcluded(atom1, atom2, exclusions, exclusionStartIndex);
#endif
                    if (includeAtom)
                        included[numIncluded++] = atom2;
                }
                
                // If we found any neighbors, store them to the neighbor list.
                
                if (numIncluded > 0) {
                    int baseIndex = atomicAdd(numNeighborPairs, numIncluded);
                    if (baseIndex+numIncluded <= maxNeighborPairs)
                        for (int i = 0; i < numIncluded; i++)
                            neighborPairs[baseIndex+i] = make_int2(atom1, included[i]);
                    totalNeighborsForAtom1 += numIncluded;
                }
            }
        }
        numNeighborsForAtom[atom1] = totalNeighborsForAtom1;
    }
}

/**
 * Sum the neighbor counts to compute the start position of each atom.  This kernel
 * is executed as a single work group.
 */
extern "C" __global__ void computeNeighborStartIndices(int* __restrict__ numNeighborsForAtom, int* __restrict__ neighborStartIndex) {
    extern __shared__ unsigned int posBuffer[];
    unsigned int globalOffset = 0;
    for (unsigned int startAtom = 0; startAtom < NUM_ATOMS; startAtom += blockDim.x) {
        // Load the neighbor counts into local memory.

        unsigned int globalIndex = startAtom+threadIdx.x;
        posBuffer[threadIdx.x] = (globalIndex < NUM_ATOMS ? numNeighborsForAtom[globalIndex] : 0);
        __syncthreads();

        // Perform a parallel prefix sum.

        for (unsigned int step = 1; step < blockDim.x; step *= 2) {
            unsigned int add = (threadIdx.x >= step ? posBuffer[threadIdx.x-step] : 0);
            __syncthreads();
            posBuffer[threadIdx.x] += add;
            __syncthreads();
        }

        // Write the results back to global memory.

        if (globalIndex < NUM_ATOMS)
            neighborStartIndex[globalIndex+1] = posBuffer[threadIdx.x]+globalOffset;
        numNeighborsForAtom[globalIndex] = 0; // Clear this so the next kernel can use it as a counter
        globalOffset += posBuffer[blockDim.x-1];
    }
    if (threadIdx.x == 0)
        neighborStartIndex[0] = 0;
}

/**
 * Assemble the final neighbor list.
 */
extern "C" __global__ void copyPairsToNeighborList(const int2* __restrict__ neighborPairs, int* __restrict__ neighbors, int* __restrict__ numNeighborPairs,
            int maxNeighborPairs, int* __restrict__ numNeighborsForAtom, const int* __restrict__ neighborStartIndex) {
    int actualPairs = *numNeighborPairs;
    if (actualPairs > maxNeighborPairs)
        return; // There wasn't enough memory for the neighbor list, so we'll need to rebuild it.
    for (unsigned int index = blockDim.x*blockIdx.x+threadIdx.x; index < actualPairs; index += blockDim.x*gridDim.x) {
        int2 pair = neighborPairs[index];
        int startIndex = neighborStartIndex[pair.x];
        int offset = atomicAdd(numNeighborsForAtom+pair.x, 1);
        neighbors[startIndex+offset] = pair.y;
    }
}
