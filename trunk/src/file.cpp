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
#include "ersky9x.h"
#include "stdio.h"
#include "inttypes.h"
#include "string.h"
#include "myeeprom.h"
#include "drivers.h"
#include "file.h"
#ifdef FRSKY
#include "frsky.h"
#endif

extern PROGMEM s9xsplash[] ;



// Logic for storing to EERPOM/loading from EEPROM
// If main needs to wait for the eeprom, call mainsequence without actioning menus
// General configuration
// 'main' set flag STORE_GENERAL
// 'eeCheck' sees flag STORE_GENERAL, copies data, starts 2 second timer, clears flag STORE_GENERAL
// 'eeCheck' sees 2 second timer expire, locks copy, initiates write, enters WRITE_GENERAL mode
// 'eeCheck' completes write, unlocks copy, exits WRITE_GENERAL mode

// 'main' set flag STORE_MODEL(n)

// 'main' set flag STORE_MODEL_TRIM(n)


// 'main' needs to load a model





// These may not be needed, or might just be smaller
uint8_t Spi_tx_buf[24] ;
uint8_t Spi_rx_buf[24] ;


struct t_file_entry File_system[MAX_MODELS+1] ;

unsigned char ModelNames[MAX_MODELS+1][sizeof(g_model.name)+1] ;		// Allow for general


uint16_t General_timer ;
uint16_t Model_timer ;
uint32_t Update_timer ;
uint8_t General_dirty ;
uint8_t Model_dirty ;

uint8_t Ee32_general_write_pending ;
uint8_t Ee32_model_write_pending ;
uint8_t Ee32_model_delete_pending ;

uint8_t	Eeprom32_process_state ;
uint8_t	Eeprom32_state_after_erase ;
uint8_t	Eeprom32_write_pending ;
uint8_t Eeprom32_file_index ;
uint8_t *Eeprom32_buffer_address ;
uint8_t *Eeprom32_source_address ;
uint32_t Eeprom32_address ;
uint32_t Eeprom32_data_size ;


#define EE_WAIT			0
#define EE_NO_WAIT	1


// States in Eeprom32_process_state
#define E32_IDLE							1
#define E32_ERASESENDING			2
#define E32_ERASEWAITING			3
#define E32_WRITESENDING			4
#define E32_WRITEWAITING			5
#define E32_READSENDING				6
#define E32_READWAITING				7
#define E32_BLANKCHECK				8
#define E32_WRITESTART				9

//#define E32_READING						10		// Set elsewhere as a lock


void handle_serial( void ) ;

bool ee32ModelExists(uint8_t id) ;
uint32_t get_current_block_number( uint32_t block_no, uint16_t *p_size, uint32_t *p_seq ) ;
uint32_t read32_eeprom_data( uint32_t eeAddress, register uint8_t *buffer, uint32_t size, uint32_t immediate ) ;
uint32_t write32_eeprom_block( uint32_t eeAddress, register uint8_t *buffer, uint32_t size, uint32_t immediate ) ;
void ee32_read_model_names( void ) ;
void ee32LoadModelName(uint8_t id, unsigned char*buf,uint8_t len) ;
void ee32_update_name( uint32_t id, uint8_t *source ) ;
void convertModel( SKYModelData *dest, ModelData *source ) ;



void eeprom_process( void ) ;
uint32_t ee32_process( void ) ;

// New file system

// Start with 16 model memories, initial test uses 34 and 35 for these
// Blocks 0 and 1, general
// Blocks 2 and 3, model 1
// Blocks 4 and 5, model 2 etc
// Blocks 32 and 33, model 16

uint8_t Current_general_block ;		// 0 or 1 is active block
uint8_t Other_general_block_blank ;

struct t_eeprom_header
{
	uint32_t sequence_no ;		// sequence # to decide which block is most recent
	uint16_t data_size ;			// # bytes in data area
	uint8_t flags ;
	uint8_t hcsum ;
} ;

// Structure of data in a block
struct t_eeprom_block
{
	struct t_eeprom_header header ;
	union
	{
		uint8_t bytes[4088] ;
		uint32_t words[1022] ;
	} data ;
} ;


#define EEPROM_BUFFER_SIZE ((sizeof(SKYModelData) + sizeof( struct t_eeprom_header ) + 3)/4)


struct t_eeprom_buffer
{
	struct t_eeprom_header header ;
	union t_eeprom_data
	{
		EEGeneral general_data ;
		ModelData model_data ;
		uint32_t words[ EEPROM_BUFFER_SIZE ] ;
	} data ;	
} Eeprom_buffer ;


// genaral data needs to be written to EEPROM
void ee32StoreGeneral()
{
	General_dirty = 1 ;
	General_timer = 500 ;		// 5 seconds timeout before writing
}


// Store model to EEPROM, trim is non-zero if this is the result of a trim change
void ee32StoreModel( uint8_t modelNumber, uint8_t trim )
{
	Model_dirty = modelNumber + 1 ;
	Model_timer = 500 ;	
	ee32_update_name( Model_dirty, (uint8_t *)&g_model ) ;		// In case it's changed
}

void ee32_delete_model( uint8_t id )
{
	uint8_t buffer[sizeof(g_model.name)+1] ;
  memset( buffer, ' ', sizeof(g_model.name) ) ;
	ee32_update_name( id + 1, buffer ) ;
	Ee32_model_delete_pending = id + 1 ;
}

void ee32_read_model_names()
{
	uint32_t i ;

	for ( i = 1 ; i <= MAX_MODELS ; i += 1 )
	{
		ee32LoadModelName( i, ModelNames[i], sizeof(g_model.name) ) ;
	}
}

void ee32_update_name( uint32_t id, uint8_t *source )
{
	uint8_t * p ;
	uint32_t i ;

	p = ModelNames[id] ;
	for ( i = 0 ; i < sizeof(g_model.name) ; i += 1 )
	{
		*p++ = *source++ ;
	}
	*p = '\0' ;
}


// Read eeprom data starting at random address
uint32_t read32_eeprom_data( uint32_t eeAddress, register uint8_t *buffer, uint32_t size, uint32_t immediate )
{
#ifdef SIMU
  assert(size);
  eeprom_pointer = eeAddress;
  eeprom_buffer_data = (char*)buffer;
  eeprom_buffer_size = size;
  eeprom_read_operation = true;
  Spi_complete = false;
  sem_post(eeprom_write_sem);
  int x;
#else
	register uint8_t *p ;
	register uint32_t x ;

	p = Spi_tx_buf ;
	*p = 3 ;		// Read command
	*(p+1) = eeAddress >> 16 ;
	*(p+2) = eeAddress >> 8 ;
	*(p+3) = eeAddress ;		// 3 bytes address
	spi_PDC_action( p, 0, buffer, 4, size ) ;
#endif

	if ( immediate )
	{
		return 0 ;		
	}
	for ( x = 0 ; x < 100000 ; x += 1  )
	{
		if ( Spi_complete )
		{
			break ;				
		}
#ifdef SIMU
    sleep(5/*ms*/);
#endif
	}
	return x ;
}


uint32_t write32_eeprom_block( uint32_t eeAddress, register uint8_t *buffer, uint32_t size, uint32_t immediate )
{
	register uint8_t *p ;
	register uint32_t x ;

	eeprom_write_enable() ;

	p = Spi_tx_buf ;
	*p = 2 ;		// Write command
	*(p+1) = eeAddress >> 16 ;
	*(p+2) = eeAddress >> 8 ;
	*(p+3) = eeAddress ;		// 3 bytes address
	spi_PDC_action( p, buffer, 0, 4, size ) ;

	if ( immediate )
	{
		return 0 ;		
	}
	for ( x = 0 ; x < 100000 ; x += 1  )
	{
		if ( Spi_complete )
		{
			break ;				
		}        			
	}
	return x ; 
}

uint8_t byte_checksum( uint8_t *p, uint32_t size )
{
	uint32_t csum ;

	csum = 0 ;
	while( size )
	{
		csum += *p++ ;
		size -= 1 ;
	}
	return csum ;
}

uint32_t ee32_check_header( struct t_eeprom_header *hptr )
{
	uint8_t csum ;

	csum = byte_checksum( ( uint8_t *) hptr, 7 ) ;
	if ( csum == hptr->hcsum )
	{
		return 1 ;
	}
	return 0 ;
}

// Pass in an even block number, this and the next block will be checked
// to see which is the most recent, the block_no of the most recent
// is returned, with the corresponding data size if required
// and the sequence number if required
uint32_t get_current_block_number( uint32_t block_no, uint16_t *p_size, uint32_t *p_seq )
{
	struct t_eeprom_header b0 ;
	struct t_eeprom_header b1 ;
  uint32_t sequence_no ;
  uint16_t size ;
	read32_eeprom_data( block_no << 12, ( uint8_t *)&b0, sizeof(b0), EE_WAIT ) ;		// Sequence # 0
	read32_eeprom_data( (block_no+1) << 12, ( uint8_t *)&b1, sizeof(b1), EE_WAIT ) ;	// Sequence # 1

	if ( ee32_check_header( &b0 ) == 0 )
	{
		b0.sequence_no = 0 ;
		b0.data_size = 0 ;
		b0.flags = 0 ;
	}

	size = b0.data_size ;
  sequence_no = b0.sequence_no ;
	if ( ee32_check_header( &b0 ) == 0 )
	{
		if ( ee32_check_header( &b1 ) != 0 )
		{
  		size = b1.data_size ;
		  sequence_no = b1.sequence_no ;
			block_no += 1 ;
		}
		else
		{
			size = 0 ;
			sequence_no = 1 ;
		}
	}
	else
	{
		if ( ee32_check_header( &b1 ) != 0 )
		{
			if ( b1.sequence_no > b0.sequence_no )
			{
	  		size = b1.data_size ;
			  sequence_no = b1.sequence_no ;
				block_no += 1 ;
			}
		}
	}
  
	if ( size == 0xFFFF )
	{
		size = 0 ;
	}
  if ( p_size )
	{
		*p_size = size ;
	}
  if ( sequence_no == 0xFFFFFFFF )
	{  
		sequence_no = 0 ;
	}
  if ( p_seq )
	{
		*p_seq = sequence_no ;
	}
//	Block_needs_erasing = erase ;		
  
	return block_no ;
}

bool ee32LoadGeneral()
{
	uint16_t size ;
	
	size = File_system[0].size ;

  memset(&g_eeGeneral, 0, sizeof(EEGeneral));

	if ( size > sizeof(EEGeneral) )
	{
		size = sizeof(EEGeneral) ;
	}

	if ( size )
	{
		read32_eeprom_data( ( File_system[0].block_no << 12) + sizeof( struct t_eeprom_header), ( uint8_t *)&g_eeGeneral, size, 0 ) ;
	}
	else
	{
		return false ;		// No data to load
	}

//  for(uint8_t i=0; i<sizeof(g_eeGeneral.ownerName);i++) // makes sure name is valid
//  {
//      uint8_t idx = char2idx(g_eeGeneral.ownerName[i]);
//      g_eeGeneral.ownerName[i] = idx2char(idx);
//  }

  if(g_eeGeneral.myVers<MDVERS)
      sysFlags |= sysFLAG_OLD_EEPROM; // if old EEPROM - Raise flag

  g_eeGeneral.myVers   =  MDVERS; // update myvers

  uint16_t sum=0;
  if(size>(sizeof(EEGeneral)-20)) for(uint8_t i=0; i<12;i++) sum+=g_eeGeneral.calibMid[i];
	else return false ;
  return g_eeGeneral.chkSum == sum;
}

void ee32WaitLoadModel(uint8_t id)
{
	// Wait for EEPROM to be idle
	if ( General_timer )
	{
		General_timer = 1 ;		// Make these happen soon
	}
	if ( Model_timer )
	{
		Model_timer = 1 ;
	}

	while( ( Eeprom32_process_state != E32_IDLE )
			|| ( General_timer )
			|| ( Model_timer ) )
	{
		mainSequence( NO_MENU ) ;		// Wait for EERPOM to be IDLE
	}

	ee32LoadModel( id ) ;		// Now get the model
}


void ee32LoadModel(uint8_t id)
{
	uint16_t size ;
	uint8_t version = 255 ;

    if(id<MAX_MODELS)
    {
			size =  File_system[id+1].size ;
			if ( sizeof(g_model) < 720 )
			{
				version = 0 ;
			}
			memset(&g_model, 0, sizeof(g_model));

			if ( size < 720 )
			{
				memset(&g_oldmodel, 0, sizeof(g_oldmodel));
				if ( size > sizeof(g_oldmodel) )
				{
					size = sizeof(g_oldmodel) ;
				}
			}
       
			if ( size > sizeof(g_model) )
			{
				size = sizeof(g_model) ;
			}
			 
      if(size<256) // if not loaded a fair amount
      {
        modelDefault(id) ;
      }
			else
			{
				if ( size < 720 )
				{
					read32_eeprom_data( ( File_system[id+1].block_no << 12) + sizeof( struct t_eeprom_header), ( uint8_t *)&g_oldmodel, size, 0 ) ;
					convertModel( &g_model, &g_oldmodel ) ;
				}
				else
				{
					read32_eeprom_data( ( File_system[id+1].block_no << 12) + sizeof( struct t_eeprom_header), ( uint8_t *)&g_model, size, 0 ) ;
				}	 
				
				if ( version != 255 )
				{
					g_model.version = 0 ;					// default version number
				}
			}

      for(uint8_t i=0; i<sizeof(g_model.name);i++) // makes sure name is valid
      {
          uint8_t idx = char2idx(g_model.name[i]);
          g_model.name[i] = idx2char(idx);
      }
//      g_model.mdVers = MDVERS; //update mdvers

#ifdef FRSKY
  FrskyAlarmSendState |= 0x40 ;		// Get RSSI Alarms
        FRSKY_setModelAlarms();
#endif
    }
}

bool ee32ModelExists(uint8_t id)
{
	return ( File_system[id+1].size > 0 ) ;
}

void ee32LoadModelName( uint8_t id, unsigned char*buf, uint8_t len )
{
	if(id<=MAX_MODELS)
  {
		memset(buf,' ',len);
		if ( File_system[id].size > sizeof(g_model.name) )
		{
			read32_eeprom_data( ( File_system[id].block_no << 12) + 8, ( uint8_t *)buf, sizeof(g_model.name), 0 ) ;
		}
  }
}


void fill_file_index()
{
	uint32_t i ;
	for ( i = 0 ; i < MAX_MODELS + 1 ; i += 1 )
	{
		File_system[i].block_no = get_current_block_number( i * 2, &File_system[i].size, &File_system[i].sequence_no ) ;
	}
}

void init_eeprom()
{
	fill_file_index() ;
	ee32_read_model_names() ;
	Eeprom32_process_state = E32_IDLE ;
}

uint32_t ee32_check_finished()
{
	if ( ( Eeprom32_process_state != E32_IDLE )
			|| ( General_timer )
			|| ( Model_timer ) )
	{
		ee32_process() ;
		return 0 ;
	}
	return 1 ;
}


uint32_t ee32_process()
{
	register uint8_t *p ;
	register uint8_t *q ;
	register uint32_t x ;
	register uint32_t eeAddress ;

//	return 0 ;

	if ( General_timer )
	{
		if ( --General_timer == 0 )
		{
			
			// Time to write g_eeGeneral
			Ee32_general_write_pending = 1 ;
		}
	}
	if ( Model_timer )
	{
		if ( --Model_timer == 0 )
		{
			
			// Time to write model
			Ee32_model_write_pending = 1 ;
		}
	}

	if ( Eeprom32_process_state == E32_IDLE )
	{
		if ( Ee32_general_write_pending )
		{
			Ee32_general_write_pending = 0 ;			// clear flag

			// Check we can write, == block is blank

			Eeprom32_source_address = (uint8_t *)&g_eeGeneral ;		// Get data fromm here
			Eeprom32_data_size = sizeof(g_eeGeneral) ;						// This much
			Eeprom32_file_index = 0 ;								// This file system entry
			Eeprom32_process_state = E32_BLANKCHECK ;
//			Writing_model = 0 ;
		}
		else if ( Ee32_model_write_pending )
		{
			Ee32_model_write_pending = 0 ;			// clear flag

			// Check we can write, == block is blank

			Eeprom32_source_address = (uint8_t *)&g_model ;		// Get data from here
			Eeprom32_data_size = sizeof(g_model) ;						// This much
			Eeprom32_file_index = Model_dirty ;								// This file system entry
			Eeprom32_process_state = E32_BLANKCHECK ;
//			Writing_model = Model_dirty ;
		}
		else if ( Ee32_model_delete_pending )
		{
			Eeprom32_source_address = (uint8_t *)&g_model ;		// Get data from here
			Eeprom32_data_size = 0 ;													// This much
			Eeprom32_file_index = Ee32_model_delete_pending ;	// This file system entry
			Ee32_model_delete_pending = 0 ;
			Eeprom32_process_state = E32_BLANKCHECK ;
		}
	}

	if ( Eeprom32_process_state == E32_BLANKCHECK )
	{
		eeAddress = File_system[Eeprom32_file_index].block_no ^ 1 ;
		eeAddress <<= 12 ;		// Block start address
		Eeprom32_address = eeAddress ;						// Where to put new data
		x = Eeprom32_data_size + sizeof( struct t_eeprom_header ) ;	// Size needing to be checked
		p = (uint8_t *) &Eeprom_buffer ;
		read32_eeprom_data( eeAddress, p, x, 1 ) ;
		Eeprom32_process_state = E32_READSENDING ;
	}

	if ( Eeprom32_process_state == E32_READSENDING )
	{
		if ( Spi_complete )
		{
			uint32_t blank = 1 ;
			x = Eeprom32_data_size + sizeof( struct t_eeprom_header ) ;	// Size needing to be checked
			p = (uint8_t *) &Eeprom_buffer ;
			while ( x )
			{
				if ( *p++ != 0xFF )
				{
					blank = 0 ;
					break ;					
				}
				x -= 1 ;
			}
			// If not blank, sort erasing here
			if ( blank )
			{
				Eeprom32_state_after_erase = E32_IDLE ;
				Eeprom32_process_state = E32_WRITESTART ;
			}
			else
			{
				eeAddress = Eeprom32_address ;
				eeprom_write_enable() ;
				p = Spi_tx_buf ;
				*p = 0x20 ;		// Block Erase command
				*(p+1) = eeAddress >> 16 ;
				*(p+2) = eeAddress >> 8 ;
				*(p+3) = eeAddress ;		// 3 bytes address
				spi_PDC_action( p, 0, 0, 4, 0 ) ;
				Eeprom32_process_state = E32_ERASESENDING ;
				Eeprom32_state_after_erase = E32_WRITESTART ;
			}
		}
	}

	if ( Eeprom32_process_state == E32_WRITESTART )
	{
		uint32_t total_size ;
		p = Eeprom32_source_address ;
		q = (uint8_t *)&Eeprom_buffer.data ;
		for ( x = 0 ; x < Eeprom32_data_size ; x += 1 )
		{
			*q++ = *p++ ;			// Copy the data to temp buffer
		}
		Eeprom_buffer.header.sequence_no = ++File_system[Eeprom32_file_index].sequence_no ;
		File_system[Eeprom32_file_index].size = Eeprom_buffer.header.data_size = Eeprom32_data_size ;
		Eeprom_buffer.header.flags = 0 ;
		Eeprom_buffer.header.hcsum = byte_checksum( (uint8_t *)&Eeprom_buffer, 7 ) ;
		total_size = Eeprom32_data_size + sizeof( struct t_eeprom_header ) ;
		eeAddress = Eeprom32_address ;		// Block start address
		x = total_size / 256 ;	// # sub blocks
		x <<= 8 ;						// to offset address
		eeAddress += x ;		// Add it in
		p = (uint8_t *) &Eeprom_buffer ;
		p += x ;						// Add offset
		x = total_size % 256 ;	// Size of last bit
		if ( x == 0 )						// Last bit empty
		{
			x = 256 ;
			p -= x ;
			eeAddress -= x ;
		}
		Eeprom32_buffer_address = p ;
		Eeprom32_address = eeAddress ;
		write32_eeprom_block( eeAddress, p, x, 1 ) ;
		Eeprom32_process_state = E32_WRITESENDING ;
	}

	if ( Eeprom32_process_state == E32_WRITESENDING )
	{
		if ( Spi_complete )
		{
			Eeprom32_process_state = E32_WRITEWAITING ;
		}			
	}		

	if ( Eeprom32_process_state == E32_WRITEWAITING )
	{
		x = eeprom_read_status() ;
		if ( ( x & 1 ) == 0 )
		{
			if ( ( Eeprom32_address & 0x0FFF ) != 0 )		// More to write
			{
				Eeprom32_address -= 256 ;
				Eeprom32_buffer_address -= 256 ;
				write32_eeprom_block( Eeprom32_address, Eeprom32_buffer_address, 256, 1 ) ;
				Eeprom32_process_state = E32_WRITESENDING ;
			}
			else
			{
				// now erase the other block
				File_system[Eeprom32_file_index].block_no ^= 1 ;		// This is now the current block
				eeAddress = Eeprom32_address ^ 0x00001000 ;		// Address of block to erase
				eeprom_write_enable() ;
				p = Spi_tx_buf ;
				*p = 0x20 ;		// Block Erase command
				*(p+1) = eeAddress >> 16 ;
				*(p+2) = eeAddress >> 8 ;
				*(p+3) = eeAddress ;		// 3 bytes address
				spi_PDC_action( p, 0, 0, 4, 0 ) ;
				Eeprom32_process_state = E32_ERASESENDING ;
				Eeprom32_state_after_erase = E32_IDLE ;
			}
		}
	}	

	if ( Eeprom32_process_state == E32_ERASESENDING )
	{
		if ( Spi_complete )
		{
			Eeprom32_process_state = E32_ERASEWAITING ;
		}			
	}	
		
	if ( Eeprom32_process_state == E32_ERASEWAITING )
	{
		x = eeprom_read_status() ;
		if ( ( x & 1 ) == 0 )
		{ // Command finished
			Eeprom32_process_state = Eeprom32_state_after_erase ;
		}			
	}

	if ( Eeprom32_process_state == E32_IDLE )
	{
		return 0 ;			// inactive
	}
	else
	{
		return 1 ;
	}
}



uint32_t unprotect_eeprom()
{
 	register uint8_t *p ;

	eeprom_write_enable() ;
		
	p = Spi_tx_buf ;
	*p = 0x39 ;		// Unprotect sector command
	*(p+1) = 0 ;
	*(p+2) = 0 ;
	*(p+3) = 0 ;		// 3 bytes address

	return spi_operation( p, Spi_rx_buf, 4 ) ;
}

void convertSwitch( int8_t *p )
{
	if ( *p <= -MAX_DRSWITCH )
	{
		*p -= (NUM_SKYCSW - NUM_CSW) ;
	}
	if ( *p >= MAX_DRSWITCH )
	{
		*p += (NUM_SKYCSW - NUM_CSW) ;
	}
}


void convertModel( SKYModelData *dest, ModelData *source )
{
	uint32_t i ;
	memset( dest, 0, sizeof(*dest) ) ;
  memcpy( dest->name, source->name, MODEL_NAME_LEN) ;
	dest->modelVoice = source->modelVoice ;
	dest->RxNum = source->RxNum ;
	dest->traineron = source->traineron ;
	dest->FrSkyUsrProto = source->FrSkyUsrProto ;
	dest->FrSkyGpsAlt = source->FrSkyGpsAlt ;
	dest->FrSkyImperial = source->FrSkyImperial ;
	dest->FrSkyAltAlarm = source->FrSkyAltAlarm ;
	dest->version = source->version ;
	dest->protocol = source->protocol ;
	dest->ppmNCH = source->ppmNCH ;
	dest->thrTrim = source->thrTrim ;
	dest->numBlades = source->numBlades ;
	dest->thrExpo = source->thrExpo ;
	dest->trimInc = source->trimInc ;
	dest->ppmDelay = source->ppmDelay ;
	dest->trimSw = source->trimSw ;
	dest->beepANACenter = source->beepANACenter ;
	dest->pulsePol = source->pulsePol ;
	dest->extendedLimits = source->extendedLimits ;
	dest->swashInvertELE = source->swashInvertELE ;
	dest->swashInvertAIL = source->swashInvertAIL ;
	dest->swashInvertCOL = source->swashInvertCOL ;
	dest->swashType = source->swashType ;
	dest->swashCollectiveSource = source->swashCollectiveSource ;
	dest->swashRingValue = source->swashRingValue ;
	dest->ppmFrameLength = source->ppmFrameLength ;
	for ( i = 0 ; i < MAX_MIXERS ; i += 1 )
	{
		MixData *src = &source->mixData[i] ;
		SKYMixData *dst = &dest->mixData[i] ;
		dst->destCh = src->destCh ;
		dst->srcRaw = src->srcRaw ;
		if ( dst->srcRaw == NUM_XCHNRAW+1 )		// MIX_3POS
		{
			dst->srcRaw += NUM_SKYCHNOUT - NUM_CHNOUT ;
		}

		dst->weight = src->weight ;
		dst->swtch = src->swtch ;
		convertSwitch( &dst->swtch ) ;
		
		dst->curve = src->curve ;
		dst->delayUp = src->delayUp * 10 ;
		dst->delayDown = src->delayDown * 10 ;
		dst->speedUp = src->speedUp * 10 ;
		dst->speedDown = src->speedDown * 10 ;
		dst->carryTrim = src->carryTrim ;
		dst->mltpx = src->mltpx ;
		dst->mixWarn = src->mixWarn ;
		dst->enableFmTrim = src->enableFmTrim ;
		dst->sOffset = src->sOffset ;
	}
	for ( i = 0 ; i < NUM_CHNOUT ; i += 1 )
	{
		dest->limitData[i] = source->limitData[i] ;
	}
	for ( i = 0 ; i < 4 ; i += 1 )
	{
		dest->expoData[i] = source->expoData[i] ;
		convertSwitch( &dest->expoData[i].drSw1 ) ;
		convertSwitch( &dest->expoData[i].drSw2 ) ;
		dest->trim[i] = source->trim[i] ;
	}
  memcpy( dest->curves5, source->curves5, sizeof(source->curves5) ) ;
  memcpy( dest->curves9, source->curves9, sizeof(source->curves9) ) ;
	for ( i = 0 ; i < NUM_CSW ; i += 1 )
	{
		CSwData *src = &source->customSw[i] ;
		SKYCSwData *dst = &dest->customSw[i] ;
		dst->v1 = src->v1 ;
		dst->v2 = src->v2 ;
		dst->func = src->func ;
  	switch (dst->func)
		{
  		case (CS_AND) :
  		case (CS_OR) :
  		case (CS_XOR) :
				convertSwitch( &dst->v1 ) ;
				convertSwitch( &dst->v2 ) ;
      break;
  		case (CS_VPOS):
  		case (CS_VNEG):
  		case (CS_APOS):
  		case (CS_ANEG):
				if ( dst->v1-1 >= CHOUT_BASE+NUM_CHNOUT )
				{
					dst->v1 += NUM_SKYCHNOUT - NUM_CHNOUT ;
				}
				if ( dst->v2-1 >= CHOUT_BASE+NUM_CHNOUT )
				{
					dst->v2 += NUM_SKYCHNOUT - NUM_CHNOUT ;
				}
      break;
		}
		dst->andsw = src->andsw ;
	}
	dest->frSkyVoltThreshold = source->frSkyVoltThreshold ;
	dest->bt_telemetry = source->bt_telemetry ;
	dest->numVoice = source->numVoice ;
	if ( dest->numVoice )
	{
		dest->numVoice += NUM_SKYCHNOUT - NUM_CHNOUT ;
	}
	for ( i = 0 ; i < NUM_CHNOUT ; i += 1 )
	{
		SafetySwData *src = &source->safetySw[i] ;
		SKYSafetySwData *dst = &dest->safetySw[i] ;
		if ( i < (uint32_t)NUM_CHNOUT - dest->numVoice )
		{
			dst->opt.ss.swtch = src->opt.ss.swtch ;
			dst->opt.ss.mode = src->opt.ss.mode ;
			dst->opt.ss.val = src->opt.ss.val ;
			if ( dst->opt.ss.mode == 3 )
			{
				dst->opt.ss.mode = 0 ;
			}
			switch ( dst->opt.ss.mode )
			{
				case 0 :
				case 1 :
				case 2 :
					convertSwitch( &dst->opt.ss.swtch ) ;
				break ;
			}
		}
		else
		{
			dst->opt.vs.vswtch = src->opt.vs.vswtch ;
			dst->opt.vs.vmode = src->opt.vs.vmode ;
			dst->opt.vs.vval = src->opt.vs.vval ;
			convertSwitch( (int8_t *)&dst->opt.vs.vswtch ) ;
		}
	}

	for ( i = 0 ; i < 2 ; i += 1 )
	{
		FrSkyChannelData *src = &source->frsky.channels[i] ;
		SKYFrSkyChannelData *dst = &dest->frsky.channels[i] ;
		dst->ratio = src->ratio ;
		dst->alarms_value[0] = src->alarms_value[0] ;
		dst->alarms_value[1] = src->alarms_value[1] ;
		dst->alarms_level = src->alarms_level ;
		dst->alarms_greater = src->alarms_greater ;
		dst->type = src->type ;
		dst->gain = 1 ;
	}

	for ( i = 0 ; i < 2 ; i += 1 )
	{
		dest->timer[i] = source->timer[i] ;
  	if( dest->timer[i].tmrModeB>=(MAX_DRSWITCH))	 //momentary on-off
		{
			dest->timer[i].tmrModeB += (NUM_SKYCSW - NUM_CSW) ;
		}
	}

	dest->frskyAlarms = source->frskyAlarms ;

  memcpy( &dest->customDisplayIndex[0], &source->customDisplayIndex[0], 6 ) ;

}



