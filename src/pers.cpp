/*
 * Author - Mike Blandford
 *
 * Based on er9x by Erez Raviv <erezraviv@gmail.com>
 *
 * Based on th9x -> http://code.google.com/p/th9x/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <stdint.h>
#include <string.h>
#include "AT91SAM3S4.h"
#include "ersky9x.h"
#include "templates.h"
#include "myeeprom.h"
#include "file.h"
#include "debug.h"

#ifdef FRSKY
#include "frsky.h"
#endif

void generalDefault()
{
  memset(&g_eeGeneral,0,sizeof(g_eeGeneral));
  g_eeGeneral.myVers   =  MDVERS;
  g_eeGeneral.currModel=  0;
  g_eeGeneral.contrast = 30;
  g_eeGeneral.vBatWarn = 65;
  g_eeGeneral.stickMode=  1;
	g_eeGeneral.disablePotScroll=  1;
	g_eeGeneral.bright = 50 ;
	g_eeGeneral.volume = 2 ;
  for (int i = 0; i < 7; ++i) {
    g_eeGeneral.calibMid[i]     = 0x400;
    g_eeGeneral.calibSpanNeg[i] = 0x300;
    g_eeGeneral.calibSpanPos[i] = 0x300;
  }
  strncpy_P(g_eeGeneral.ownerName,PSTR("ME        "), 10);
  int16_t sum=0;
  for(int i=0; i<12;i++) sum+=g_eeGeneral.calibMid[i];
  g_eeGeneral.chkSum = sum;
}

void modelDefault(uint8_t id)
{
  memset(&g_model, 0, sizeof(ModelData));
  strncpy_P(g_model.name,PSTR("MODEL     "), 10 );
  g_model.name[5]='0'+(id+1)/10;
  g_model.name[6]='0'+(id+1)%10;
//  g_model.mdVers = MDVERS;

  applyTemplate(0); //default 4 channel template
}

bool eeDuplicateModel(uint8_t id)
{
  uint32_t i;
  for( i=id+1; i<MAX_MODELS; i++)
  {
    if(! ee32ModelExists(i) ) break;
  }
  if(i==MAX_MODELS) return false; //no free space in directory left

	ee32StoreModel( i, 0 ) ;

	return true ;
}


void eeReadAll()
{
//	txmit('a') ;
  if(!ee32LoadGeneral() )
  {
//	txmit('b') ;
		
    alert((char const *)"Bad EEprom Data", true);
    g_eeGeneral.contrast = 25 ;
    message(PSTR("EEPROM Formatting"));
    generalDefault();

    modelDefault(0);
    
		STORE_GENERALVARS;
    STORE_MODELVARS;        
  }
	else
	{
  	ee32LoadModel(g_eeGeneral.currModel);
	}
}


void eeDirty(uint8_t msk)
{
  if(!msk) return;

	// New file system operations
	if ( msk & EE_GENERAL )
	{
		ee32StoreGeneral() ;
	}
	if ( msk & EE_MODEL )
	{
		ee32StoreModel( g_eeGeneral.currModel, msk & EE_TRIM ) ;
	}

}


