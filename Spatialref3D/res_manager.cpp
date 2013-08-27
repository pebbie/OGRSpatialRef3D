#include "res_manager.h"
#include "cpl_port.h"
#include "interpolation.h"

#define RAD_TO_DEG	57.29577951308232
#define MAXINT 9999999
#define MAXEXTENT 1024		// maximum window size

#define INSIDE(x,y,l,t,w,h) (x>=l && x<=l+w && y>=t && y<=t+h)

RasterResampler::RasterResampler() : bIsSmall(false), 
									nRasterWidth(0), 
									nRasterHeight(0),
									nWndXOffset(0),
									nWndYOffset(0),
									nWndWidth(0),
									nWndHeight(0)
{
	padWindow = NULL;
	poData = NULL;
}

RasterResampler::~RasterResampler()
{
	Cleanup();

	if(poData != NULL)
		GDALClose(poData);
}

double
	RasterResampler::GetValueAt(double x, double y)
{
	double dPixel = x;
	double dLine = y;
	MapToRaster(&dPixel, &dLine);

	// check if buffer not initialized or point not inside current window
	// naive caching strategy
	if (padWindow == NULL 
				|| !INSIDE((int)dPixel, (int)dLine, nWndXOffset, nWndYOffset, nWndWidth, nWndHeight)){

		int nWndLeft = (int)dPixel-MAXEXTENT/2;
		int nWndTop = (int)dLine-MAXEXTENT/2;

		Request(MAX(0, nWndLeft), MAX(0, nWndTop), 
				MIN(nRasterWidth-1, nWndLeft+MAXEXTENT), 
				MIN(nRasterHeight-1, nWndTop+MAXEXTENT));
	}

	return GetValueResampled(dPixel, dLine);
}

void
	RasterResampler::GetValueAt(int point_count, double *x, double *y, double *z)
{
	// dummy naive approach 
	for(int i=0; i<point_count; ++i)
		z[i] = GetValueAt(x[i], y[i]);

	// indexed (UNTESTED)
	double* padX = (double*)CPLMalloc(sizeof(double)*point_count);
	double* padY = (double*)CPLMalloc(sizeof(double)*point_count);

	double dXMin = MAXINT;
	double dYMin = MAXINT;
	double dXMax = -MAXINT;
	double dYMax = -MAXINT;

	// convert all point's coordinate
	// from map coordinate to 
	// raster coordinate
	for(int i=0; i<point_count; ++i){
		double px = x[i];
		double py = y[i];

		MapToRaster(&px, &py);

		padX[i] = px;
		padY[i] = py;

		dXMin = MIN(dXMin, px);
		dYMin = MIN(dYMin, py);
		dXMax = MAX(dXMax, px);
		dYMax = MAX(dYMax, py);
	}

	double dWidth = dXMax-dXMin;
	double dHeight = dYMax-dYMin;

	if (bIsSmall || (dWidth < MAXEXTENT && dHeight < MAXEXTENT)){
		// do it in single patch
		// use naive approach embedded in single point interface

		// TODO: add checking to window boundary against raster boundary
		Request((int)dXMin, (int)dYMin, (int)dWidth+2, (int)dHeight+2);

		for(int i=0; i<point_count; ++i){
			z[i] = GetValueResampled(padX[i], padY[i]);
		}
	}
	else{
		// both raster and window size not small enough to be all in memory
		// do it by window patch
		bool* panIdx = (bool*)CPLMalloc(sizeof(bool)*point_count);
		int unprocessed_count = point_count;	// # of point unprocessed

		for(int i=0; i<point_count; ++i)
				panIdx[i] = false;
		
		while(unprocessed_count < point_count){
			//initialize next cache
			for(int i=0; i<point_count; ++i){
				if(!panIdx[i]){
					if (padWindow == NULL){
				
						int nWndLeft = (int)padX[i]-MAXEXTENT/2;
						int nWndTop = (int)padY[i]-MAXEXTENT/2;

						Request(MAX(0, nWndLeft), MAX(0, nWndTop), 
								MIN(nRasterWidth-1, nWndLeft+MAXEXTENT), 
								MIN(nRasterHeight-1, nWndTop+MAXEXTENT));
					}

					break;
				}
			}

			// process all applicable points
			for(int i=0; i<point_count; ++i){
				// skip processed points or points outside current cache window
				if(panIdx[i]
					|| !INSIDE((int)padX[i], (int)padY[i], nWndXOffset, nWndYOffset, nWndWidth, nWndHeight)) 
						continue;

				z[i] = GetValueResampled(padX[i], padY[i]);
				panIdx[i] = true;	// mark processed
				unprocessed_count += 1;
			}
		}// endwhile
		
		CPLFree(panIdx);
	}

	CPLFree(padX);
	CPLFree(padY);
}

OGRErr
	RasterResampler::Open(const char *pszFilename)
{
	sFilename = pszFilename;
	poData = (GDALDataset *) GDALOpen( pszFilename, GA_ReadOnly );
	
	if( poData == NULL )
    {
        printf("gdal failed - unable to open '%s'.\n",
                 pszFilename );
		return OGRERR_FAILURE;
	}

	Prepare();
	return OGRERR_NONE;
}

const char*
	RasterResampler::GetFilename()
{
	return sFilename;
}

double
	RasterResampler::GetValueResampled(double x, double y)
/*
 * x and y is assumed to be in raster coordinate (not buffer window coordinate)
 */
{
	int px = (int)floor(x);
	int py = (int)floor(y);

	// Boundary checking
	if (px < 0 || py < 0 || px >= nRasterWidth || py >= nRasterHeight)
		throw std::exception( "point outside raster." );
	else {
		double dx = x - px;
		double dy = y - py;

		// TODO: accomodate neigbor acquisition as required by other interpolation function (e.g. bicubic)
		// acquire neighbors
		int offset = (py-nWndYOffset)*nWndWidth+(px-nWndXOffset);
		double p[4];
		p[0] = padWindow[offset];
		p[1] = padWindow[offset+1];

		offset += nWndWidth;
		p[2] = padWindow[offset];
		p[3] = padWindow[offset+1];

		return bilinearInterpolation(p, dx, dy, dNoDataValue);
	}

	return 0.0;
}

void
	RasterResampler::Cleanup()
{
	if(padWindow != NULL)
		CPLFree(padWindow);
	padWindow = NULL;
}

void
	RasterResampler::Prepare()
	/*
	 * get metadata information from raster file
	 */
{
	nRasterWidth = poData->GetRasterXSize();
    nRasterHeight = poData->GetRasterYSize();
	bIsSmall = (nRasterWidth < MAXEXTENT) && (nRasterHeight > MAXEXTENT);

	dNoDataValue = poData->GetRasterBand(1)->GetNoDataValue();
	double geotrans[6];
    poData->GetGeoTransform(geotrans);

	if( GDALInvGeoTransform( geotrans, dInvGeotrans ) == 0 )
      throw std::exception( "inversion of geo transformation failed." );
}

void
	RasterResampler::Request(int left, int top, int width, int height)
	/*
	 * Access pixel data from raster file to temporary buffer (padWindow)
	 */
{
	nWndXOffset = left;
	nWndYOffset = top;
	nWndWidth = width;
	nWndHeight = height;
	
	Cleanup();

	int nWndArea = width*height;
	padWindow = (double *) CPLMalloc(sizeof(double)*nWndArea);

	poData->RasterIO( GF_Read, 
						nWndXOffset, nWndYOffset, nWndWidth, nWndHeight, 
                        padWindow, nWndWidth, nWndHeight, GDT_Float64, 
                        1, NULL, 0, 0, 0 );
}

void 
	RasterResampler::MapToRaster(double *x, double *y)
	/* 
	 * converts map coordinate (radian) to raster coordinate (pixels)
	 */
{
	*(x) = dInvGeotrans[0]+ 
          + *(x) * dInvGeotrans[1] * RAD_TO_DEG
          + *(y) * dInvGeotrans[2] * RAD_TO_DEG;

    *(y) = dInvGeotrans[3]
          + *(x) * dInvGeotrans[4] * RAD_TO_DEG
          + *(y) * dInvGeotrans[5] * RAD_TO_DEG;
}