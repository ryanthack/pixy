#include <stdio.h>
#include <debug.h>
#include <string.h>
#include "pixy_init.h"
#include "camera.h"
#include "cameravals.h"
#include "param.h"
#include "conncomp.h"
#include "blobs.h"
#include "qqueue.h"


Qqueue *g_qqueue;
Blobs *g_blobs;

int g_loop = 0;

int32_t cc_servo(const uint32_t &start)
{
	g_loop = start;
	return 0;
}

static const ProcModule g_module[] =
{
	{
	"cc_getRLSFrame",
	(ProcPtr)cc_getRLSFrameChirp, 
	{END}, 
	"Get a frame of color run-length segments (RLS)"
	"@r 0 if success, negative if error"
	"@r CCQ1 formated data, including 8-palette"
	},
	{
	"cc_setSigRegion",
	(ProcPtr)cc_setSigRegion, 
	{CRP_UINT8, CRP_HTYPE(FOURCC('R','E','G','1')), END}, 
	"Set model by selecting region in image"
	"@p model numerical index of model, can be 1-7"
	"@p region user-selected region"
	"@r 0 to 100 if success where 100=good, 0=poor, negative if error"
	},
	{
	"cc_setSigPoint",
	(ProcPtr)cc_setSigPoint, 
	{CRP_UINT8, CRP_HTYPE(FOURCC('P','N','T','1')), END}, 
	"Set model by selecting point in image"
	"@p model numerical index of model, can be 1-7"
	"@p point user-selected point"
	"@r 0 to 100 if success where 100=good, 0=poor, negative if error"
	},
	{
	"cc_setMemory",
	(ProcPtr)cc_setMemory,
	{CRP_UINT32, CRP_UINTS8, END},
	"" 
	},
	END
};

static ChirpProc g_getRLSFrameM0 = -1;

int cc_loadLut(void)
{
	int i, res;
	uint32_t len;
	char id[32];
	ColorModel *pmodel;

	// indicate that raw frame has been overwritten
	g_rawFrame.m_pixels = NULL;
	// clear lut
	g_blobs->m_clut->clear();

	for (i=1; i<=NUM_MODELS; i++)
	{
		sprintf(id, "signature%d", i);
		// get signature and add to color lut
		res = prm_get(id, &len, &pmodel, END);
		if (res<0)
			return res;
		g_blobs->m_clut->add(pmodel, i);
	}

	// go ahead and flush since we've changed things
	g_qqueue->flush();

	return 0;
}

void cc_setupSignatures(void)
{
	int i;
	ColorModel model;
	char id[32], desc[32];

	for (i=1; i<=NUM_MODELS; i++)
	{
		sprintf(id, "signature%d", i);
		sprintf(desc, "Color signature %d", i);
		// add if it doesn't exist yet
		prm_add(id, desc, INTS8(sizeof(ColorModel), &model), END);
	}
}

int cc_init(Chirp *chirp)
{
	g_qqueue = new Qqueue;
	g_blobs = new Blobs(g_qqueue);

	chirp->registerModule(g_module);	

	g_getRLSFrameM0 = g_chirpM0->getProc("getRLSFrame", NULL);

	if (g_getRLSFrameM0<0)
		return -1;

	cc_setupSignatures(); // setup default vals (if necessary)
	cc_loadLut(); // load lut from flash

	return 0;
}

// this routine assumes it can grab valid pixels in video memory described by the box
int32_t cc_setSigRegion(const uint8_t &model, const uint16_t &xoffset, const uint16_t &yoffset, const uint16_t &width, const uint16_t &height)
{
	int result;
	char id[32];
	ColorModel cmodel;

	if (model<1 || model>NUM_MODELS)
		return -1;

	// create lut
	result = g_blobs->generateLUT(model, g_rawFrame, RectA(xoffset, yoffset, width, height), &cmodel);
	if (result<0)
		return result;

	// save to flash
	sprintf(id, "signature%d", model);
	prm_set(id, INTS8(sizeof(ColorModel), &cmodel), END);

	return result;
}

int32_t cc_setSigPoint(const uint8_t &model, const uint16_t &x, const uint16_t &y, Chirp *chirp)
{
	RectA region;
	int result; 
	char id[32];
	ColorModel cmodel;

	if (model<1 || model>NUM_MODELS)
		return -1;

	result = g_blobs->generateLUT(model, g_rawFrame, Point16(x, y), &cmodel, &region);
  	if (result<0)
		return result;

	if (chirp)
	{
		BlobA blob(model, region.m_xOffset, region.m_xOffset+region.m_width, region.m_yOffset, region.m_yOffset+region.m_height);
		cc_sendBlobs(chirp, &blob, 1, RENDER_FLAG_FLUSH | RENDER_FLAG_BLEND_BG);
	}

	// save to flash
	sprintf(id, "signature%d", model);
	prm_set(id, INTS8(sizeof(ColorModel), &cmodel), END);

	return result;
}

int32_t cc_getRLSFrameChirp(Chirp *chirp)
{
	return cc_getRLSFrameChirpFlags(chirp);
}

int32_t cc_getRLSFrameChirpFlags(Chirp *chirp, uint8_t renderFlags)
{
	int32_t result;
	uint32_t len, numRls;

	if (g_rawFrame.m_pixels)
		cc_loadLut();

	g_qqueue->flush();

	// figure out prebuf length (we need the prebuf length and the number of runlength segments, but there's a chicken and egg problem...)
	len = Chirp::serialize(chirp, RLS_MEMORY, RLS_MEMORY_SIZE,  HTYPE(0), UINT16(0), UINT16(0), UINTS32_NO_COPY(0), END);

	result = cc_getRLSFrame((uint32_t *)(RLS_MEMORY+len), LUT_MEMORY);
	// copy from IPC memory to RLS_MEMORY
	numRls = g_qqueue->readAll((Qval *)(RLS_MEMORY+len), (RLS_MEMORY_SIZE-len)/sizeof(Qval));
	Chirp::serialize(chirp, RLS_MEMORY, RLS_MEMORY_SIZE,  HTYPE(FOURCC('C','C','Q','1')), HINT8(renderFlags), UINT16(CAM_RES2_WIDTH), UINT16(CAM_RES2_HEIGHT), UINTS32_NO_COPY(numRls), END);
	// send frame, use in-place buffer
	chirp->useBuffer(RLS_MEMORY, len+numRls*4);

	return result;
}

int32_t cc_getRLSFrame(uint32_t *memory, uint8_t *lut, bool sync)
{
	int32_t res;
	int32_t responseInt = -1;

	// check mode, set if necessary
	if ((res=cam_setMode(CAM_MODE1))<0)
		return res;

	// forward call to M0, get frame
	if (sync)
	{
		g_chirpM0->callSync(g_getRLSFrameM0, 
			UINT32((uint32_t)memory), UINT32((uint32_t)lut), END_OUT_ARGS,
			&responseInt, END_IN_ARGS);
		return responseInt;
	}
	else
	{
		g_chirpM0->callAsync(g_getRLSFrameM0, 
			UINT32((uint32_t)memory), UINT32((uint32_t)lut), END_OUT_ARGS);
		return 0;
	}

}

int32_t cc_setMemory(const uint32_t &location, const uint32_t &len, const uint8_t *data)
{
	uint32_t i;
	uint8_t *dest = (uint8_t *)location;
	for (i=0; i<len; i++)
		dest[i] = data[i];

	return len;
}

int cc_sendBlobs(Chirp *chirp, const BlobA *blobs, uint32_t len, uint8_t renderFlags)
{
	CRP_RETURN(chirp, HTYPE(FOURCC('C','C','B','1')), HINT8(renderFlags), HINT16(CAM_RES2_WIDTH), HINT16(CAM_RES2_HEIGHT), UINTS16(len*sizeof(BlobA)/sizeof(uint16_t), blobs), END);
	return 0;
}

