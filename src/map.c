/*
	This file is part of Warzone 2100.
	Copyright (C) 1999-2004  Eidos Interactive
	Copyright (C) 2005-2007  Warzone Resurrection Project

	Warzone 2100 is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	Warzone 2100 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Warzone 2100; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
*/
/*
 * Map.c
 *
 * Utility functions for the map data structure.
 *
 */
#include <stdio.h>
#include <string.h>

/* map line printf's */
#include <assert.h>
#include "lib/framework/frame.h"
#include "lib/framework/frameint.h"
#include "map.h"
#include "lib/gamelib/gtime.h"
#include "hci.h"
#include "projectile.h"
#include "display3d.h"
#include "lighting.h"
#include "game.h"

#include "environ.h"
#include "advvis.h"

#include "gateway.h"
#include "wrappers.h"

#include "lib/framework/fractions.h"

//scroll min and max values
SDWORD		scrollMinX, scrollMaxX, scrollMinY, scrollMaxY;

/* Structure definitions for loading and saving map data */
typedef struct _map_save_header
{
	char		aFileType[4];
	UDWORD		version;
	UDWORD		width;
	UDWORD		height;
} MAP_SAVEHEADER;


#define SAVE_MAP_V2 \
	UWORD		texture; \
	UBYTE		height

typedef struct _map_save_tilev2
{
	SAVE_MAP_V2;
} MAP_SAVETILEV2;

typedef struct _map_save_tile
{
	SAVE_MAP_V2;
} MAP_SAVETILE;

typedef struct _gateway_save_header
{
	UDWORD		version;
	UDWORD		numGateways;
} GATEWAY_SAVEHEADER;

typedef struct _gateway_save
{
	UBYTE	x0,y0,x1,y1;
} GATEWAY_SAVE;

typedef struct _zonemap_save_header_v1 {
	UWORD version;
	UWORD numZones;
} ZONEMAP_SAVEHEADER_V1;

typedef struct _zonemap_save_header {
	UWORD version;
	UWORD numZones;
	UWORD numEquivZones;
	UWORD pad;
} ZONEMAP_SAVEHEADER;

/* Sanity check definitions for the save struct file sizes */
#define SAVE_HEADER_SIZE	16
#define SAVE_TILE_SIZE		3
#define SAVE_TILE_SIZEV1	6
#define SAVE_TILE_SIZEV2	3

// Maximum expected return value from get height
#define	MAX_HEIGHT			(256 * ELEVATION_SCALE)

/* Number of entries in the sqrt(1/(1+x*x)) table for aaLine */
#define	ROOT_TABLE_SIZE		1024

/* aaLine direction bits and tables */
#define DIR_STEEP			1  /* set when abs(dy) > abs(dx) */
#define DIR_NEGY			2  /* set whey dy < 0 */

/* Defines to access the map for aaLine */
#define PIXADDR(x,y)		mapTile(x,y)
#define PIXINC(dx,dy)		((dy * mapWidth) + dx)

/* The size and contents of the map */
UDWORD	mapWidth = 0, mapHeight = 0;
MAPTILE	*psMapTiles = NULL;

/* The map tiles generated by map calc line */
TILE_COORD			*aMapLinePoints = NULL;

/* Look up table that returns the terrain type of a given tile texture */
UBYTE terrainTypes[MAX_TILE_TEXTURES];

/* pointer to a load map function - depends on version */
BOOL (*pLoadMapFunc)(char *pFileData, UDWORD fileSize);


MAPTILE *GetCurrentMap(void)	// returns a pointer to the current loaded map data
{
	return(psMapTiles);
}

/* Create a new map of a specified size */
BOOL mapNew(UDWORD width, UDWORD height)
{
//	UDWORD	numPoints;
	UDWORD	i;
	MAPTILE	*psTile;

	/* See if a map has already been allocated */
	if (psMapTiles != NULL)
	{
		/* Clear all the objects off the map and free up the map memory */
		freeAllDroids();
		freeAllStructs();
		freeAllFeatures();
		proj_FreeAllProjectiles();
//		free(psMapTiles);
		free(aMapLinePoints);
		psMapTiles = NULL;
		aMapLinePoints = NULL;
	}

	if (width*height > MAP_MAXAREA)
	{
		debug( LOG_ERROR, "mapNew: map too large : %d %d\n", width, height );
		abort();
		return FALSE;
	}

	psMapTiles = (MAPTILE *)malloc(sizeof(MAPTILE) * width*height);
	if (psMapTiles == NULL)
	{
		debug( LOG_ERROR, "mapNew: Out of memory" );
		abort();
		return FALSE;
	}
	memset(psMapTiles, 0, sizeof(MAPTILE) * width*height);

	mapWidth = width;
	mapHeight = height;

	for (i=0; i<MAX_TILE_TEXTURES; i++)
	{
		terrainTypes[i] = TER_SANDYBRUSH;
	}

	/* Allocate a buffer for the LOS routines points */

/*	numPoints = sqrtf(mapWidth * mapWidth +  mapHeight * mapHeight) + 1;

	aMapLinePoints = (TILE_COORD *)malloc(sizeof(TILE_COORD) * numPoints);
	if (!aMapLinePoints)
	{
		DBERROR(("Out of memory"));
		return FALSE;
	}
	maxLinePoints = numPoints;
*/
	intSetMapPos(mapWidth * TILE_UNITS/2, mapHeight * TILE_UNITS/2);

	/* Initialise the map terrain type */
	psTile = psMapTiles;
	/*
	for(i=mapWidth * mapHeight; i>0; i--)
	{
		psTile->type = TER_GRASS;
		psTile++;
	}
	*/

	environReset();

	/*set up the scroll mins and maxs - set values to valid ones for a new map*/
	scrollMinX = scrollMinY = 0;
	scrollMaxX = mapWidth;
	scrollMaxY = mapHeight;
	return TRUE;
}

/* load the map data - for version 3 */
static BOOL mapLoadV3(char *pFileData, UDWORD fileSize)
{
	UDWORD				i,j;
	MAP_SAVETILEV2		*psTileData;
	GATEWAY_SAVEHEADER	*psGateHeader;
	GATEWAY_SAVE		*psGate;
	ZONEMAP_SAVEHEADER	*psZoneHeader;
	UWORD ZoneSize;
	UBYTE *pZone;
	UBYTE *pDestZone;

	/* Load in the map data */
	psTileData = (MAP_SAVETILEV2 *)(pFileData + SAVE_HEADER_SIZE);
	for(i=0; i< mapWidth * mapHeight; i++)
	{
		/* MAP_SAVETILEV2 */
		endian_uword(&psTileData->texture);

		psMapTiles[i].texture = psTileData->texture;
//		psMapTiles[i].type = psTileData->type;
		psMapTiles[i].height = psTileData->height;
		// Changed line - alex
		//end of change - alex
		for (j=0; j<MAX_PLAYERS; j++)
		{
			psMapTiles[i].tileVisBits =(UBYTE)(( (psMapTiles[i].tileVisBits) &~ (UBYTE)(1<<j) ));
		}
		psTileData = (MAP_SAVETILEV2 *)(((UBYTE *)psTileData) + SAVE_TILE_SIZE);
	}


	psGateHeader = (GATEWAY_SAVEHEADER*)psTileData;
	psGate = (GATEWAY_SAVE*)(psGateHeader+1);

	/* GATEWAY_SAVEHEADER */
	endian_udword(&psGateHeader->version);
	endian_udword(&psGateHeader->numGateways);

	ASSERT( psGateHeader->version == 1,"Invalid gateway version" );

	for(i=0; i<psGateHeader->numGateways; i++) {
		if (!gwNewGateway(psGate->x0,psGate->y0, psGate->x1,psGate->y1)) {
			debug( LOG_ERROR, "mapLoadV3: Unable to add gateway" );
			abort();
			return FALSE;
		}
		psGate++;
	}

//	if (!gwProcessMap())
//	{
//		return FALSE;
//	}
//
//	if ((psGateways != NULL) &&
//		!gwGenerateLinkGates())
//	{
//		return FALSE;
//	}
	psZoneHeader = (ZONEMAP_SAVEHEADER*)psGate;

	/* ZONEMAP_SAVEHEADER */
	endian_uword(&psZoneHeader->version);
	endian_uword(&psZoneHeader->numZones);
	endian_uword(&psZoneHeader->numEquivZones);
	endian_uword(&psZoneHeader->pad);

	ASSERT( (psZoneHeader->version == 1) || (psZoneHeader->version == 2),
			"Invalid zone map version" );

	if(!gwNewZoneMap()) {
		return FALSE;
	}

	// This is a bit nasty but should work fine.
	if(psZoneHeader->version == 1) {
		// version 1 so add the size of a version 1 header.
		pZone = ((UBYTE*)psZoneHeader) + sizeof(ZONEMAP_SAVEHEADER_V1);
	} else {
		// version 2 so add the size of a version 2 header.
		pZone = ((UBYTE*)psZoneHeader) + sizeof(ZONEMAP_SAVEHEADER);
	}

	for(i=0; i<psZoneHeader->numZones; i++) {
		ZoneSize = *((UWORD*)(pZone));
		endian_uword(&ZoneSize);

		pDestZone = gwNewZoneLine(i,ZoneSize);

		if(pDestZone == NULL) {
			return FALSE;
		}

		for(j=0; j<ZoneSize; j++) {
			pDestZone[j] = pZone[2+j];
		}

		pZone += ZoneSize+2;
	}

	// Version 2 has the zone equivelancy lists tacked on the end.
	if(psZoneHeader->version == 2) {

		if(psZoneHeader->numEquivZones > 0) {
			// Load in the zone equivelance lists.
			if(!gwNewEquivTable(psZoneHeader->numEquivZones)) {
				debug( LOG_ERROR, "gwNewEquivTable failed" );
				abort();
				return FALSE;
			}

			for(i=0; i<psZoneHeader->numEquivZones; i++) {
				if(*pZone != 0) {
					if(!gwSetZoneEquiv(i, (SDWORD)*pZone, pZone+1)) {
						debug( LOG_ERROR, "gwSetZoneEquiv failed" );
						abort();
						return FALSE;
					}
				}
				pZone += ((UDWORD)*pZone)+1;
			}
		}
	}

	if ((char *)pZone - pFileData > fileSize)
	{
		debug( LOG_ERROR, "mapLoadV3: unexpected end of file" );
		abort();
		return FALSE;
	}

	LOADBARCALLBACK();	//	loadingScreenCallback();

	if ((apEquivZones != NULL) &&
		!gwGenerateLinkGates())
	{
		return FALSE;
	}

	LOADBARCALLBACK();	//	loadingScreenCallback();

	//add new map initialise
	if (!gwLinkGateways())
	{
		return FALSE;
	}

	LOADBARCALLBACK();	//	loadingScreenCallback();

#if defined(DEBUG)
	gwCheckZoneSizes();
#endif

	return TRUE;
}


/* Initialise the map structure */
BOOL mapLoad(char *pFileData, UDWORD fileSize)
{
	UDWORD				width,height;
	MAP_SAVEHEADER		*psHeader;
	BOOL				mapAlloc;

	/* Check the file type */
	psHeader = (MAP_SAVEHEADER *)pFileData;
	if (psHeader->aFileType[0] != 'm' || psHeader->aFileType[1] != 'a' ||
		psHeader->aFileType[2] != 'p' || psHeader->aFileType[3] != ' ')
	{
		debug( LOG_ERROR, "mapLoad: Incorrect file type" );
		abort();
		free(pFileData);
		return FALSE;
	}

	/* MAP_SAVEHEADER */
	endian_udword(&psHeader->version);
	endian_udword(&psHeader->width);
	endian_udword(&psHeader->height);

	/* Check the file version - deal with version 1 files */
	/* Check the file version */
	if (psHeader->version <= VERSION_9)
	{
		ASSERT(FALSE, "MapLoad: unsupported save format version %d", psHeader->version);
		free(pFileData);
		return FALSE;
	}
	else if (psHeader->version <= CURRENT_VERSION_NUM)
	{
		pLoadMapFunc = mapLoadV3;	// Includes gateway data for routing.
	}
	else
	{
		ASSERT(FALSE, "MapLoad: undefined save format version %d", psHeader->version);
		free(pFileData);
		return FALSE;
	}

	/* Get the width and height */
	width = psHeader->width;
	height = psHeader->height;

	if (width*height > MAP_MAXAREA)
	{
		debug( LOG_ERROR, "mapLoad: map too large : %d %d\n", width, height );
		abort();
		return FALSE;
	}

	/* See if this is the first time a map has been loaded */
	mapAlloc = TRUE;
	if (psMapTiles != NULL)
	{
		if (mapWidth == width && mapHeight == height)
		{
			mapAlloc = FALSE;
		}
		else
		{
			/* Clear all the objects off the map and free up the map memory */
			freeAllDroids();
			freeAllStructs();
			freeAllFeatures();
			proj_FreeAllProjectiles();
//			free(psMapTiles);
			free(aMapLinePoints);
			psMapTiles = NULL;
			aMapLinePoints = NULL;
		}
	}

	/* Allocate the memory for the map */
	if (mapAlloc)
	{
		psMapTiles = (MAPTILE *)malloc(sizeof(MAPTILE) * width*height);
		if (psMapTiles == NULL)
		{
			debug( LOG_ERROR, "mapLoad: Out of memory" );
			abort();
			return FALSE;
		}
		memset(psMapTiles, 0, sizeof(MAPTILE) * width*height);

		mapWidth = width;
		mapHeight = height;

/*		a terrain type is loaded when necessary - so don't reset
		for (i=0; i<MAX_TILE_TEXTURES; i++)
		{
			terrainTypes[i] = TER_SANDYBRUSH;
		}*/

		/* Allocate a buffer for the LOS routines points */

/*		numPoints = sqrtf(mapWidth * mapWidth +  mapHeight * mapHeight) + 1;

		aMapLinePoints = (TILE_COORD *)malloc(sizeof(TILE_COORD) * numPoints);
		if (!aMapLinePoints)
		{
			DBERROR(("Out of memory"));
			return FALSE;
		}
		maxLinePoints = numPoints;
*/
//		intSetMapPos(mapWidth * TILE_UNITS/2, mapHeight * TILE_UNITS/2);
	}

	//load in the map data itself
	pLoadMapFunc(pFileData, fileSize);

	environReset();

	/* set up the scroll mins and maxs - set values to valid ones for any new map */
	scrollMinX = scrollMinY = 0;
	scrollMaxX = mapWidth;
	scrollMaxY = mapHeight;

	return TRUE;
}


/* Save the map data */
BOOL mapSave(char **ppFileData, UDWORD *pFileSize)
{
	UDWORD	i;
	MAP_SAVEHEADER	*psHeader = NULL;
	MAP_SAVETILE	*psTileData = NULL;
	MAPTILE	*psTile = NULL;
	GATEWAY *psCurrGate = NULL;
	GATEWAY_SAVEHEADER *psGateHeader = NULL;
	GATEWAY_SAVE *psGate = NULL;
	ZONEMAP_SAVEHEADER *psZoneHeader = NULL;
	UBYTE *psZone = NULL;
	UBYTE *psLastZone = NULL;
	SDWORD	numGateways = 0;

	// find the number of non water gateways
	for(psCurrGate = gwGetGateways(); psCurrGate; psCurrGate = psCurrGate->psNext)
	{
		if (!(psCurrGate->flags & GWR_WATERLINK))
		{
			numGateways += 1;
		}
	}


	/* Allocate the data buffer */
	*pFileSize = SAVE_HEADER_SIZE + mapWidth*mapHeight * SAVE_TILE_SIZE;
	// Add on the size of the gateway data.
	*pFileSize += sizeof(GATEWAY_SAVEHEADER) + sizeof(GATEWAY_SAVE)*numGateways;
	// Add on the size of the zone data header.
	*pFileSize += sizeof(ZONEMAP_SAVEHEADER);
	// Add on the size of the zone data.
	for(i=0; i<gwNumZoneLines(); i++) {
		*pFileSize += 2+gwZoneLineSize(i);
	}
	// Add on the size of the equivalency lists.
	for(i=0; i<(UDWORD)gwNumZones; i++) {
		*pFileSize += 1+aNumEquiv[i];
	}

	*ppFileData = (char*)malloc(*pFileSize);
	if (*ppFileData == NULL)
	{
		debug( LOG_ERROR, "Out of memory" );
		abort();
		return FALSE;
	}

	/* Put the file header on the file */
	psHeader = (MAP_SAVEHEADER *)*ppFileData;
	psHeader->aFileType[0] = 'm';
	psHeader->aFileType[1] = 'a';
	psHeader->aFileType[2] = 'p';
	psHeader->aFileType[3] = ' ';
	psHeader->version = CURRENT_VERSION_NUM;
	psHeader->width = mapWidth;
	psHeader->height = mapHeight;

	/* MAP_SAVEHEADER */
	endian_udword(&psHeader->version);
	endian_udword(&psHeader->width);
	endian_udword(&psHeader->height);

	/* Put the map data into the buffer */
	psTileData = (MAP_SAVETILE *)(*ppFileData + SAVE_HEADER_SIZE);
	psTile = psMapTiles;
	for(i=0; i<mapWidth*mapHeight; i++)
	{

		// don't save the noblock flag as it gets set again when the objects are loaded
		psTileData->texture = (UWORD)(psTile->texture & (UWORD)~TILE_NOTBLOCKING);

		psTileData->height = psTile->height;

		/* MAP_SAVETILEV2 */
		endian_uword(&psTileData->texture);

		psTileData = (MAP_SAVETILE *)((UBYTE *)psTileData + SAVE_TILE_SIZE);
		psTile ++;
	}

	// Put the gateway header.
	psGateHeader = (GATEWAY_SAVEHEADER*)psTileData;
	psGateHeader->version = 1;
	psGateHeader->numGateways = numGateways;

	/* GATEWAY_SAVEHEADER */
	endian_udword(&psGateHeader->version);
	endian_udword(&psGateHeader->numGateways);

	psGate = (GATEWAY_SAVE*)(psGateHeader+1);

	i=0;
	// Put the gateway data.
	for(psCurrGate = gwGetGateways(); psCurrGate; psCurrGate = psCurrGate->psNext)
	{
		if (!(psCurrGate->flags & GWR_WATERLINK))
		{
			psGate->x0 = psCurrGate->x1;
			psGate->y0 = psCurrGate->y1;
			psGate->x1 = psCurrGate->x2;
			psGate->y1 = psCurrGate->y2;
			psGate++;
			i++;
		}
	}

	// Put the zone header.
	psZoneHeader = (ZONEMAP_SAVEHEADER*)psGate;
	psZoneHeader->version = 2;
	psZoneHeader->numZones =(UWORD)gwNumZoneLines();
	psZoneHeader->numEquivZones =(UWORD)gwNumZones;

	/* ZONEMAP_SAVEHEADER */
	endian_uword(&psZoneHeader->version);
	endian_uword(&psZoneHeader->numZones);
	endian_uword(&psZoneHeader->numEquivZones);
	endian_uword(&psZoneHeader->pad);

	// Put the zone data.
	psZone = (UBYTE*)(psZoneHeader+1);
	for(i=0; i<gwNumZoneLines(); i++) {
		psLastZone = psZone;
		*((UWORD*)psZone) = (UWORD)gwZoneLineSize(i);
		endian_uword(((UWORD *) psZone));

		psZone += sizeof(UWORD);
		memcpy(psZone,apRLEZones[i],gwZoneLineSize(i));
		psZone += gwZoneLineSize(i);
	}

	// Put the equivalency lists.
	if(gwNumZones > 0) {
		for(i=0; i<(UDWORD)gwNumZones; i++) {
			psLastZone = psZone;
			*psZone = aNumEquiv[i];
			psZone ++;
			if(aNumEquiv[i]) {
				memcpy(psZone,apEquivZones[i],aNumEquiv[i]);
				psZone += aNumEquiv[i];
			}
		}
	}

	ASSERT( ( ((UDWORD)psLastZone) - ((UDWORD)*ppFileData) ) < *pFileSize,"Buffer overflow saving map" );

	return TRUE;
}

/* Shutdown the map module */
BOOL mapShutdown(void)
{
	if(psMapTiles)
	{
		free(psMapTiles);
	}
	psMapTiles = NULL;
	mapWidth = mapHeight = 0;

	return TRUE;
}

/* Return linear interpolated height of x,y */
extern SWORD map_Height(UDWORD x, UDWORD y)
{
	SDWORD	retVal;
	UDWORD tileX, tileY, tileYOffset;
	UDWORD tileX2, tileY2Offset;
	SDWORD h0, hx, hy, hxy, wTL = 0, wTR = 0, wBL = 0, wBR = 0;
	//SDWORD	lowerHeightOffset,upperHeightOffset;
	SDWORD dx, dy, ox, oy;
	BOOL	bWaterTile = FALSE;

	// Print out a debug message when we get SDWORDs passed as if they're UDWORDs
	if (x > SDWORD_MAX)
		debug(LOG_ERROR, "map_Height: x coordinate is a negative SDWORD passed as an UDWORD: %d", (SDWORD)x);
	if (y > SDWORD_MAX)
		debug(LOG_ERROR, "map_Height: y coordinate is a negative SDWORD passed as an UDWORD: %d", (SDWORD)y);

	x = x > SDWORD_MAX ? 0 : x;//negative SDWORD passed as UDWORD
	x = x >= world_coord(mapWidth) ? world_coord(mapWidth - 1) : x;
	y = y > SDWORD_MAX ? 0 : y;//negative SDWORD passed as UDWORD
	y = y >= world_coord(mapHeight) ? world_coord(mapHeight - 1) : y;

	/* Turn into tile coordinates */
	tileX = map_coord(x);
	tileY = map_coord(y);

	/* Inter tile comp */
	ox = map_round(x);
	oy = map_round(y);

	if(TERRAIN_TYPE(mapTile(tileX,tileY)) == TER_WATER)
	{
		bWaterTile = TRUE;
		wTL = environGetValue(tileX,tileY)/2;
		wTR = environGetValue(tileX+1,tileY)/2;
		wBL = environGetValue(tileX,tileY+1)/2;
		wBR = environGetValue(tileX+1,tileY+1)/2;
		/*
		lowerHeightOffset = waves[(y%(MAX_RIPPLES-1))];
		upperHeightOffset = waves[((y%(MAX_RIPPLES-1))+1)];
		oy = (SDWORD)y - world_coord(tileY);
		oy = TILE_UNITS - oy;
		dy = ((lowerHeightOffset - upperHeightOffset) * oy )/ TILE_UNITS;

		return((SEA_LEVEL + (dy*ELEVATION_SCALE)));
		*/
	}

	// to account for the border of the map
	if(tileX + 1 < mapWidth)
	{
		tileX2 = tileX + 1;
	}
	else
	{
		tileX2 = tileX;
	}
	tileYOffset = (tileY * mapWidth);
	if(tileY + 1 < mapHeight)
	{
		tileY2Offset = tileYOffset + mapWidth;
	}
	else
	{
		tileY2Offset = tileYOffset;
	}

	ASSERT( ox < TILE_UNITS, "mapHeight: x offset too big" );
	ASSERT( oy < TILE_UNITS, "mapHeight: y offset too big" );
	ASSERT( ox >= 0, "mapHeight: x offset too small" );
	ASSERT( oy >= 0, "mapHeight: y offset too small" );

	//different code for 4 different triangle cases
	if (psMapTiles[tileX + tileYOffset].texture & TILE_TRIFLIP)
	{
		if ((ox + oy) > TILE_UNITS)//tile split top right to bottom left object if in bottom right half
		{
			ox = TILE_UNITS - ox;
			oy = TILE_UNITS - oy;
			hy = psMapTiles[tileX + tileY2Offset].height;
			hx = psMapTiles[tileX2 + tileYOffset].height;
			hxy= psMapTiles[tileX2 + tileY2Offset].height;
			if(bWaterTile)
			{
				hy+=wBL;
				hx+=wTR;
				hxy+=wBR;
			}

			dx = ((hy - hxy) * ox )/ TILE_UNITS;
			dy = ((hx - hxy) * oy )/ TILE_UNITS;

			retVal = (SDWORD)(((hxy + dx + dy)) * ELEVATION_SCALE);
			ASSERT( retVal<MAX_HEIGHT,"Map height's gone weird!!!" );
			return ((SWORD)retVal);
		}
		else //tile split top right to bottom left object if in top left half
		{
			h0 = psMapTiles[tileX + tileYOffset].height;
			hy = psMapTiles[tileX + tileY2Offset].height;
			hx = psMapTiles[tileX2 + tileYOffset].height;

			if(bWaterTile)
			{
				h0+=wTL;
				hy+=wBL;
				hx+=wTR;
			}
			dx = ((hx - h0) * ox )/ TILE_UNITS;
			dy = ((hy - h0) * oy )/ TILE_UNITS;

			retVal = (SDWORD)((h0 + dx + dy) * ELEVATION_SCALE);
			ASSERT( retVal<MAX_HEIGHT,"Map height's gone weird!!!" );
			return ((SWORD)retVal);
		}
	}
	else
	{
		if (ox > oy) //tile split topleft to bottom right object if in top right half
		{
			h0 = psMapTiles[tileX + tileYOffset].height;
			hx = psMapTiles[tileX2 + tileYOffset].height;
			ASSERT( tileX2 + tileY2Offset < mapWidth*mapHeight, "array out of bounds");
			hxy= psMapTiles[tileX2 + tileY2Offset].height;

			if(bWaterTile)
			{
				h0+=wTL;
				hx+=wTR;
				hxy+=wBR;
			}
			dx = ((hx - h0) * ox )/ TILE_UNITS;
			dy = ((hxy - hx) * oy )/ TILE_UNITS;
			retVal = (SDWORD)(((h0 + dx + dy)) * ELEVATION_SCALE);
			ASSERT( retVal<MAX_HEIGHT,"Map height's gone weird!!!" );
			return ((SWORD)retVal);
		}
		else //tile split topleft to bottom right object if in bottom left half
		{
			h0 = psMapTiles[tileX + tileYOffset].height;
			hy = psMapTiles[tileX + tileY2Offset].height;
			hxy = psMapTiles[tileX2 + tileY2Offset].height;

			if(bWaterTile)
			{
				h0+=wTL;
				hy+=wBL;
				hxy+=wBR;
			}
			dx = ((hxy - hy) * ox )/ TILE_UNITS;
			dy = ((hy - h0) * oy )/ TILE_UNITS;

			retVal = (SDWORD)((h0 + dx + dy) * ELEVATION_SCALE);
			ASSERT( retVal<MAX_HEIGHT,"Map height's gone weird!!!" );
			return ((SWORD)retVal);
		}
	}
	return 0;
}

/* returns TRUE if object is above ground */
extern BOOL mapObjIsAboveGround( BASE_OBJECT *psObj )
{
	SDWORD	iZ,
			tileX = map_coord(psObj->x),
			tileY = map_coord(psObj->y),
			tileYOffset1 = (tileY * mapWidth),
			tileYOffset2 = ((tileY+1) * mapWidth),
			h1 = psMapTiles[tileYOffset1 + tileX    ].height,
			h2 = psMapTiles[tileYOffset1 + tileX + 1].height,
			h3 = psMapTiles[tileYOffset2 + tileX    ].height,
			h4 = psMapTiles[tileYOffset2 + tileX + 1].height;

	/* trivial test above */
	if ( (psObj->z > h1) && (psObj->z > h2) &&
		 (psObj->z > h3) && (psObj->z > h4)    )
	{
		return TRUE;
	}

	/* trivial test below */
	if ( (psObj->z <= h1) && (psObj->z <= h2) &&
		 (psObj->z <= h3) && (psObj->z <= h4)    )
	{
		return FALSE;
	}

	/* exhaustive test */
	iZ = map_Height( psObj->x, psObj->y );
	if ( psObj->z > iZ )
	{
		return TRUE;
	}
	else
	{
		return FALSE;
	}
}

/* returns the max and min height of a tile by looking at the four corners
   in tile coords */
void getTileMaxMin(UDWORD x, UDWORD y, UDWORD *pMax, UDWORD *pMin)
{
	UDWORD	height, i, j;

	*pMin = TILE_MAX_HEIGHT;
	*pMax = TILE_MIN_HEIGHT;

	for (j=0; j < 2; j++)
	{
		for (i=0; i < 2; i++)
		{
			height = map_TileHeight(x+i, y+j);
			if (*pMin > height)
			{
				*pMin = height;
			}
			if (*pMax < height)
			{
				*pMax = height;
			}
		}
	}
}

UDWORD GetWidthOfMap(void)
{
	return mapWidth;
}



UDWORD GetHeightOfMap(void)
{
	return mapHeight;
}


// -----------------------------------------------------------------------------------
/* This will save out the visibility data */
bool writeVisibilityData(const char* fileName)
{
	unsigned int i;
	VIS_SAVEHEADER fileHeader;

	PHYSFS_file* fileHandle = openSaveFile(fileName);
	if (!fileHandle)
	{
		return false;
	}

	fileHeader.aFileType[0] = 'v';
	fileHeader.aFileType[1] = 'i';
	fileHeader.aFileType[2] = 's';
	fileHeader.aFileType[3] = 'd';

	fileHeader.version = CURRENT_VERSION_NUM;

	// Write out the current file header
	if (PHYSFS_write(fileHandle, fileHeader.aFileType, sizeof(fileHeader.aFileType), 1) != 1
	 || !PHYSFS_writeUBE32(fileHandle, fileHeader.version))
	{
		debug(LOG_ERROR, "writeVisibilityData: could not write header to %s; PHYSFS error: %s", fileName, PHYSFS_getLastError());
		PHYSFS_close(fileHandle);
		return false;
	}

	for (i = 0; i < mapWidth * mapHeight; ++i)
	{
		if (!PHYSFS_writeUBE8(fileHandle, psMapTiles[i].tileVisBits))
		{
			debug(LOG_ERROR, "writeVisibilityData: could not write to %s; PHYSFS error: %s", fileName, PHYSFS_getLastError());
			PHYSFS_close(fileHandle);
			return false;
		}
	}

	// Everything is just fine!
	return true;
}

// -----------------------------------------------------------------------------------
/* This will read in the visibility data */
bool readVisibilityData(const char* fileName)
{
	VIS_SAVEHEADER fileHeader;
	unsigned int expectedFileSize, fileSize;
	unsigned int i;

	PHYSFS_file* fileHandle = openLoadFile(fileName, false);
	if (!fileHandle)
	{
		// Failure to open the file is no failure to read it
		return true;
	}

	// Read the header from the file
	if (PHYSFS_read(fileHandle, fileHeader.aFileType, sizeof(fileHeader.aFileType), 1) != 1
	 || !PHYSFS_readUBE32(fileHandle, &fileHeader.version))
	{
		debug(LOG_ERROR, "readVisibilityData: error while reading header from file: %s", PHYSFS_getLastError());
		PHYSFS_close(fileHandle);
		return false;
	}

	// Check the header to see if we've been given a file of the right type
	if (fileHeader.aFileType[0] != 'v'
	 || fileHeader.aFileType[1] != 'i'
	 || fileHeader.aFileType[2] != 's'
	 || fileHeader.aFileType[3] != 'd')
	{
		debug(LOG_ERROR, "readVisibilityData: Weird file type found? Has header letters - '%c' '%c' '%c' '%c' (should be 'v' 'i' 's' 'd')",
		      fileHeader.aFileType[0],
		      fileHeader.aFileType[1],
		      fileHeader.aFileType[2],
		      fileHeader.aFileType[3]);

		PHYSFS_close(fileHandle);
		return false;
	}

	// Validate the filesize
	expectedFileSize = sizeof(fileHeader.aFileType) + sizeof(fileHeader.version) + mapWidth * mapHeight * sizeof(uint8_t);
	fileSize = PHYSFS_fileLength(fileHandle);
	if (fileSize != expectedFileSize)
	{
		PHYSFS_close(fileHandle);
		ASSERT(!"readVisibilityData: unexpected filesize", "readVisibilityData: unexpected filesize; should be %u, but is %u", expectedFileSize, fileSize);
		abort();
		return false;
	}

	// For every tile...
	for(i=0; i<mapWidth*mapHeight; i++)
	{
		/* Get the visibility data */
		if (!PHYSFS_readUBE8(fileHandle, &psMapTiles[i].tileVisBits))
		{
			debug(LOG_ERROR, "readVisibilityData: could not read from %s; PHYSFS error: %s", fileName, PHYSFS_getLastError());
			PHYSFS_close(fileHandle);
			return false;
		}
	}

	// Close the file
	PHYSFS_close(fileHandle);

	/* Hopefully everything's just fine by now */
	return true;
}
// -----------------------------------------------------------------------------------
