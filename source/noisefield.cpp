/******************************************************************************
 *
 * MantaFlow fluid solver framework
 * Copyright 2011 Tobias Pfaff, Nils Thuerey 
 *
 * This program is free software, distributed under the terms of the
 * GNU General Public License (GPL) 
 * http://www.gnu.org/licenses
 *
 * Noise field
 *
 ******************************************************************************/

#include "noisefield.h"
#include "randomstream.h"
#include "grid.h"

using namespace std;

//*****************************************************************************
// Wavelet noise 


namespace Manta {

int WaveletNoiseField::randomSeed = 13322223;
float* WaveletNoiseField::mNoiseTile = NULL;

static Real _aCoeffs[32] = {
    0.000334f,-0.001528f, 0.000410f, 0.003545f,-0.000938f,-0.008233f, 0.002172f, 0.019120f,
    -0.005040f,-0.044412f, 0.011655f, 0.103311f,-0.025936f,-0.243780f, 0.033979f, 0.655340f,
    0.655340f, 0.033979f,-0.243780f,-0.025936f, 0.103311f, 0.011655f,-0.044412f,-0.005040f,
    0.019120f, 0.002172f,-0.008233f,-0.000938f, 0.003546f, 0.000410f,-0.001528f, 0.000334f};

void WaveletNoiseField::downsample(Real *from, Real *to, int n, int stride){
    const Real *a = &_aCoeffs[16];
    for (int i = 0; i < n / 2; i++) {
        to[i * stride] = 0;
        for (int k = 2 * i - 16; k < 2 * i + 16; k++) {
            to[i * stride] += a[k - 2 * i] * from[modFast128(k) * stride];
        }
    }
}

static Real _pCoeffs[4] = {0.25f, 0.75f, 0.75f, 0.25f};

void WaveletNoiseField::upsample(Real *from, Real *to, int n, int stride) {
    const Real *pp = &_pCoeffs[1];

    for (int i = 0; i < n; i++) {
        to[i * stride] = 0;
        for (int k = i / 2 - 1 ; k < i / 2 + 3; k++) {
            to[i * stride] += 0.5 * pp[k - i / 2] * from[modSlow(k, n / 2) * stride];
        } // new */
    }
}

WaveletNoiseField::WaveletNoiseField(FluidSolver* parent, int fixedSeed, int loadFromFile) :
    PbClass(parent), mPosOffset(0.), mPosScale(1.), mValOffset(0.), mValScale(1.), mClamp(false), 
    mClampNeg(0), mClampPos(1), mTimeAnim(0), mGsInvX(0), mGsInvY(0), mGsInvZ(0)
{
    mGsInvX = 1.0/(parent->getGridSize().x);
    mGsInvY = 1.0/(parent->getGridSize().y);
    mGsInvZ = parent->is3D() ? (1.0/(parent->getGridSize().z)) : 1;

    // use global random seed with offset if none is given
    if (fixedSeed==-1) {
        fixedSeed = randomSeed + 123;
    }
    RandomStream randStreamPos(fixedSeed);
    mSeedOffset = Vec3( randStreamPos.getVec3Norm() );

    generateTile( loadFromFile );
};

string WaveletNoiseField::toString() {
    std::ostringstream out;
    out <<  "NoiseField: name '"<<mName<<"' "<<
        "  pos off="<<mPosOffset<<" scale="<<mPosScale<<
        "  val off="<<mValOffset<<" scale="<<mValScale<<
        "  clamp ="<<mClamp<<" val="<<mClampNeg<<" to "<<mClampPos<<
		"  timeAni ="<<mTimeAnim<<
        "  gridInv ="<<Vec3(mGsInvX,mGsInvY,mGsInvZ) ;
    return out.str();
}

void WaveletNoiseField::generateTile( int loadFromFile) {
    // generate tile
    const int n = NOISE_TILE_SIZE;
    const int n3 = n*n*n;

    if(mNoiseTile) return;

    Real *noise3 = new Real[n3];
    if(loadFromFile) {
        FILE* fp = fopen("waveletNoiseTile.bin","rb"); 
        if(fp) {
            fread(noise3, sizeof(Real), n3, fp); 
            fclose(fp);
            cout << "noise tile loaded from file! " << endl;
            mNoiseTile = noise3;
            return;
        }
    }

    cout << "generating " << n << "^3 noise tile " << endl;
    Real *temp13 = new Real[n3];
    Real *temp23 = new Real[n3];

    // initialize
    for (int i = 0; i < n3; i++) {
        temp13[i] = temp23[i] =
            noise3[i] = 0.;
    }

    // Step 1. Fill the tile with random numbers in the range -1 to 1.
    RandomStream randStreamTile ( randomSeed );
    for (int i = 0; i < n3; i++) {
        //noise3[i] = (randStream.getFloat() + randStream2.getFloat()) -1.; // produces repeated values??
        noise3[i] = randStreamTile.getRandNorm(0,1);
    }

    // Steps 2 and 3. Downsample and upsample the tile
    for (int iy = 0; iy < n; iy++) 
        for (int iz = 0; iz < n; iz++) {
            const int i = iy * n + iz*n*n;
            downsample(&noise3[i], &temp13[i], n, 1);
            upsample  (&temp13[i], &temp23[i], n, 1);
        }
    for (int ix = 0; ix < n; ix++) 
        for (int iz = 0; iz < n; iz++) {
            const int i = ix + iz*n*n;
            downsample(&temp23[i], &temp13[i], n, n);
            upsample  (&temp13[i], &temp23[i], n, n);
        }
    for (int ix = 0; ix < n; ix++) 
        for (int iy = 0; iy < n; iy++) {
            const int i = ix + iy*n;
            downsample(&temp23[i], &temp13[i], n, n*n);
            upsample  (&temp13[i], &temp23[i], n, n*n);
        }

    // Step 4. Subtract out the coarse-scale contribution
    for (int i = 0; i < n3; i++) { 
        noise3[i] -= temp23[i];
    }

    // Avoid even/odd variance difference by adding odd-offset version of noise to itself.
    int offset = n / 2;
    if (offset % 2 == 0) offset++;

    if (n != 128) errMsg("WaveletNoise::Fast 128 mod used, change for non-128 resolution");
    
    int icnt=0;
    for (int ix = 0; ix < n; ix++)
        for (int iy = 0; iy < n; iy++)
            for (int iz = 0; iz < n; iz++) { 
                temp13[icnt] = noise3[modFast128(ix+offset) + modFast128(iy+offset)*n + modFast128(iz+offset)*n*n];
                icnt++;
            }

    for (int i = 0; i < n3; i++) {
        noise3[i] += temp13[i];
    }
    
    mNoiseTile = noise3;
    delete[] temp13;
    delete[] temp23;
    
    if(loadFromFile) {
        FILE* fp = fopen("waveletNoiseTile.bin","wb"); 
        if(fp) {
            fwrite(noise3, sizeof(Real), n3, fp); 
            fclose(fp);
            cout << "saved to file! " << endl;
        }
    }
}



void WaveletNoiseField::downsampleNeumann(const float *from, float *to, int n, int stride)
{
    // if these values are not local incorrect results are generated
    static const float *const aCoCenter= &_aCoeffs[16];
    for (int i = 0; i < n / 2; i++) {
        to[i * stride] = 0;
        for (int k = 2 * i - 16; k < 2 * i + 16; k++) { 
            // handle boundary
            float fromval; 
            if (k < 0) {
                fromval = from[0];
            } else if(k > n - 1) {
                fromval = from[(n - 1) * stride];
            } else {
                fromval = from[k * stride]; 
            } 
            to[i * stride] += aCoCenter[k - 2 * i] * fromval; 
        }
    }
}

void WaveletNoiseField::upsampleNeumann(const float *from, float *to, int n, int stride) {
    static const float *const pp = &_pCoeffs[1];
    for (int i = 0; i < n; i++) {
        to[i * stride] = 0;
        for (int k = i / 2 - 1 ; k < i / 2 + 3; k++) {
            float fromval;
            if(k>n/2-1) {
                fromval = from[(n/2-1) * stride];
            } else if(k < 0) {
                fromval = from[0];
            } else {
                fromval = from[k * stride]; 
            }  
            to[i * stride] += 0.5 * pp[k - i / 2] * fromval; 
        }
    }
}

void WaveletNoiseField::computeCoefficients(Grid<Real>& input, Grid<Real>& tempIn1, Grid<Real>& tempIn2) 
{
    // generate tile
    const int sx = input.getSizeX();
    const int sy = input.getSizeY();
    const int sz = input.getSizeZ();
    const int n3 = sx*sy*sz;
    // just for compatibility with wavelet turb code
    Real *temp13 = &tempIn1(0,0,0);
    Real *temp23 = &tempIn2(0,0,0);
    Real *noise3 = &input(0,0,0);

    // clear grids
    for (int i = 0; i < n3; i++) {
        temp13[i] = temp23[i] = 0.f;
    }

    // Steps 2 and 3. Downsample and upsample the tile
    for (int iz = 0; iz < sz; iz++) 
        for (int iy = 0; iy < sy; iy++) 
        {
            const int i = iz*sx*sy + iy*sx;
            downsampleNeumann(&noise3[i], &temp13[i], sx, 1 );
            upsampleNeumann  (&temp13[i], &temp23[i], sx, 1);
        }

    for (int iz = 0; iz < sz; iz++) 
        for (int ix = 0; ix < sx; ix++) 
        {
            const int i = iz*sx*sy + ix;
            downsampleNeumann(&temp23[i], &temp13[i], sy, sx );
            upsampleNeumann  (&temp13[i], &temp23[i], sy, sx );
        }

    if(input.is3D()) {
    for (int iy = 0; iy < sy; iy++) 
        for (int ix = 0; ix < sx; ix++) 
        {
            const int i = iy*sx+ix;
            downsampleNeumann(&temp23[i], &temp13[i], sz, sy*sx );
            upsampleNeumann  (&temp13[i], &temp23[i], sz, sy*sx );
        }
    }

    // Step 4. Subtract out the coarse-scale contribution
    for (int i = 0; i < n3; i++) { 
        Real residual = noise3[i] - temp23[i];
        temp13[i] = sqrtf( fabs(residual) );
    }

    // copy back, and compute actual weight for wavelet turbulence...
    Real smoothingFactor = 1./6.;
    if(!input.is3D()) smoothingFactor = 1./4.;
    FOR_IJK_BND(input,1) {
        // apply some brute force smoothing
        Real res = temp13[k*sx*sy+j*sx+i-1] + temp13[k*sx*sy+j*sx+i+1];
        res     += temp13[k*sx*sy+j*sx+i-sx] + temp13[k*sx*sy+j*sx+i+sx];
        if( input.is3D()) res += temp13[k*sx*sy+j*sx+i-sx*sy] + temp13[k*sx*sy+j*sx+i+sx*sy];
        input(i,j,k) = res * smoothingFactor;
    }
}




    
}
