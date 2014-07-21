#pragma NOIV               // Do not generate interrupt vectors
/*
 * This file is part of the DSLogic-firmware project.
 *
 * Copyright (C) 2014 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "include/fx2.h"
#include "include/fx2regs.h"
#include "include/syncdly.h"    // SYNCDELAY macro, see Section 15.14 of FX2 Tech.
                                // Ref. Manual for usage details.
#include "include/DSLogic.h"

#define GPIFTRIGWR 0
#define GPIFTRIGRD 4

#define GPIF_EP2 0
#define GPIF_EP4 1
#define GPIF_EP6 2
#define GPIF_EP8 3

extern BOOL GotSUD;             // Received setup data flag
extern BOOL Rwuen;
extern BOOL Selfpwr;

BYTE Configuration;                 // Current configuration
BYTE AlternateSetting;              // Alternate settings
BOOL in_enable = FALSE;             // flag to enable IN transfers
BOOL enum_high_speed = FALSE;       // flag to let firmware know FX2 enumerated at high speed
BOOL cfg_enable = FALSE;
BOOL cfg_init = FALSE;
BOOL set_enable = FALSE;
BOOL set_dso_ctrl = FALSE;
BYTE command = 0;
BYTE setting_count_b0;
BYTE setting_count_b1;
BYTE setting_count_b2;
BYTE xdata dsoConfig[4] = {0x00, 0xc8, 0x61, 0x55};

//-----------------------------------------------------------------------------
// Task Dispatcher hooks
//   The following hooks are called by the task dispatcher.
//-----------------------------------------------------------------------------

void setup_gpif_waveforms(void);
void init_capture_intf(void);
void init_config_intf(void);
void start_capture(void);
void stop_capture(void);
void poll_intf(void);

void DSLogic_Init(void)
{
  // CLKSPD[1:0]=10, for 48MHz operation
  CPUCS = 0x12;                 
  SYNCDELAY;
  
  // Setup Endpoints
  SYNCDELAY;  
  EP2CFG = 0xA0;     // EP2OUT, bulk, size 512, 2x buffered
  SYNCDELAY;           
  EP6CFG = 0xE0;     // EP6IN, bulk, size 512, 2x buffered
  SYNCDELAY;
  
  // Reset FIFOs
  FIFORESET = 0x80;  // set NAKALL bit to NAK all transfers from host
  SYNCDELAY;
  FIFORESET = 0x02;  // reset EP2 FIFO
  SYNCDELAY;
  FIFORESET = 0x06;  // reset EP6 FIFO
  SYNCDELAY;
  FIFORESET = 0x00;  // clear NAKALL bit to resume normal operation
  SYNCDELAY;

  // EP2 Configuration
  EP2FIFOCFG = 0x00; 	// allow core to see zero to one transition of auto out bit
  SYNCDELAY;
  EP2FIFOCFG = 0x10; 	// auto out mode, disable PKTEND zero length send, word ops
  SYNCDELAY;
  EP2GPIFFLGSEL = 0x01; // For EP2OUT, GPIF uses Empty flag
  SYNCDELAY;

  // EP6 Configuration
  EP6FIFOCFG = 0x08; 	// auto in mode, disable PKTEND zero length send, word ops
  SYNCDELAY;
  EP6AUTOINLENH = 0x02;	// Auto-commit 512 (0x200) byte packets (due to AUTOIN = 1)
  SYNCDELAY;
  EP6AUTOINLENL = 0x00;
  SYNCDELAY;
  EP6GPIFFLGSEL = 0x02;	// For EP6IN, Set the GPIF flag to 'full'
  SYNCDELAY;
  
  // PA1~PA0: LED; PA2: Sample Enable; PA3: Sample Clear
  OEA = 0x0f;
  IOA = 0x00;
  
  // GPIF Wavedata Init
  setup_gpif_waveforms( );

  // I2C Interface Init
  EZUSB_InitI2C();
}

void DSLogic_Poll(void)
{
  if (cfg_init && EP0BCL == sizeof(struct cmd_cfg_count))
  {
    cfg_init = FALSE;
    init_config_intf();   
    FIFORESET = 0x80;  // set NAKALL bit to NAK all transfers from host
    SYNCDELAY;
    FIFORESET = 0x02;  // reset EP2 FIFO
    SYNCDELAY;
    FIFORESET = 0x06;  // reset EP6 FIFO
    SYNCDELAY;
    FIFORESET = 0x00;  // clear NAKALL bit to resume normal operation
    SYNCDELAY;	 

    EP2FIFOCFG = 0x00; // allow core to see zero to one transition of auto out bit
    SYNCDELAY;
    EP2FIFOCFG = 0x10; // auto out mode, disable PKTEND zero length send, word ops
    SYNCDELAY;
    EP6FIFOCFG = 0x08; // auto in mode, disable PKTEND zero length send, word ops
    SYNCDELAY;   	 

    GPIFIDLECTL &= 0xFB;	//PROG_B signal low
    EZUSB_Delay1ms();		//PROG_B signal kept asserted for 1 ms
    GPIFIDLECTL |= 0x04;	//PROG_B signal high
    SYNCDELAY;

    // setup transaction count
    GPIFTCB0 = ((const struct cmd_cfg_count *)EP0BUF)->byte0;   
    SYNCDELAY;
    GPIFTCB1 = ((const struct cmd_cfg_count *)EP0BUF)->byte1;            		
    SYNCDELAY;
    GPIFTCB2 = ((const struct cmd_cfg_count *)EP0BUF)->byte2;
    SYNCDELAY;

    cfg_enable = TRUE;			
  }  
  
  if (cfg_enable && (GPIFTRIG & 0x80)) 		// if GPIF interface IDLE
  {        		
    if ( (EP24FIFOFLGS & 0x01) && (GPIFREADYSTAT & 0x01)) {
      // if there's a packet in the peripheral domain for EP2
      // and FPGA is ready to receive the configuration bitstream
      IFCONFIG = 0xA6;
      // 7	IFCLKSRC=1   , FIFOs executes on internal clk source
      // 6	xMHz=0       , 30MHz internal clk rate
      // 5	IFCLKOE=1    , Drive IFCLK pin signal at 30MHz
      // 4	IFCLKPOL=0   , Don't invert IFCLK pin signal from internal clk
      // 3	ASYNC=0      , master samples asynchronous
      // 2	GSTATE=1     , Drive GPIF states out on PORTE[2:0], debug WF
      // 1:0	IFCFG=10, FX2 in GPIF master mode
      SYNCDELAY;

      //delay(1);				//avoid CSI_B deasserted during sync words
      GPIFTRIG = GPIFTRIGWR | GPIF_EP2;  	// launch GPIF FIFO WRITE Transaction from EP2 FIFO
      SYNCDELAY;

      while( !( GPIFTRIG & 0x80 ) );      	// poll GPIFTRIG.7 GPIF Done bit
      SYNCDELAY;
      cfg_enable= FALSE;                 	//end of configuration

      /* Put the FX2 into GPIF master mode and setup the GPIF. */
      //init_capture_intf();

      if (GPIFREADYSTAT & 0x02) {	// FPGA Configure Done
        IOA |= 0x01;
        IOA &= 0xf5;
        EZUSB_Delay1ms();
        IOA |= 0x08;
      } else {
        IOA &= 0xfc;
      }
    }	
  }

  switch(command)
  {
    case CMD_START:
	{
      if ((EP0CS & bmEPBUSY) != 0)
        break;
      if (EP0BCL == sizeof(struct cmd_start))
	  {
        if ((*(BYTE *)EP0BUF) & CMD_START_FLAGS_STOP)
          stop_capture();
        else
          start_capture();
      }
      command = 0;
      break;	
	}

	case CMD_SETTING:
    {
	  if ((EP0CS & bmEPBUSY) != 0)
        break;
 	  if (EP0BCL == sizeof(struct cmd_setting_count))
	  {
	    GPIFABORT = 0xff;
		SYNCDELAY;
		EP2FIFOCFG = 0x11; // auto out mode, disable PKTEND zero length send, word operation
		SYNCDELAY;
        setting_count_b0 = ((const struct cmd_setting_count *)EP0BUF)->byte0;
        setting_count_b1 = ((const struct cmd_setting_count *)EP0BUF)->byte1;
        setting_count_b2 = ((const struct cmd_setting_count *)EP0BUF)->byte2;
        set_enable = TRUE;
	  }
	  command = 0;
	  break;
    }

    case CMD_CONTROL:
    {
      if ((EP0CS & bmEPBUSY) != 0)
        break;
      if (EP0BCL == sizeof(struct cmd_control))
	  {
        dsoConfig[0] = ((const struct cmd_control *)EP0BUF)->byte0;
        dsoConfig[1] = ((const struct cmd_control *)EP0BUF)->byte1;
        dsoConfig[2] = ((const struct cmd_control *)EP0BUF)->byte2;
        dsoConfig[3] = ((const struct cmd_control *)EP0BUF)->byte3;
        set_dso_ctrl = TRUE;
      }
	  command = 0;
	  break;
    }

	default:
	  command = 0;
	  break;
  }

  if (set_enable && (GPIFTRIG & 0x80)) {	// if GPIF interface IDLE
    if (!(EP24FIFOFLGS & 0x02)) {
      SYNCDELAY;
      GPIFTCB2 = setting_count_b2;   
      SYNCDELAY;
      GPIFTCB1 = setting_count_b1;			// fpga setting count
      SYNCDELAY;
      GPIFTCB0 = setting_count_b0;
      SYNCDELAY;

      GPIFTRIG = GPIFTRIGWR | GPIF_EP2;  	// launch GPIF FIFO WRITE Transaction from EP2 FIFO
      SYNCDELAY;

      while( !( GPIFTRIG & 0x80 ) );      	// poll GPIFTRIG.7 GPIF Done bit
      SYNCDELAY;
      set_enable= FALSE;                 	//end of configuration

      /* Put the FX2 into GPIF master mode and setup the GPIF. */
      init_capture_intf();
    }	
  }

  if (set_dso_ctrl) {
    EZUSB_WriteI2C(0x51, 4, dsoConfig);
    set_dso_ctrl = FALSE;
  }

  poll_intf();  
}

//-----------------------------------------------------------------------------
// Device Request hooks
//   The following hooks are called by the end point 0 device request parser.
//-----------------------------------------------------------------------------

BOOL DR_GetDescriptor(void)
{
   return(TRUE);
}

BOOL DR_SetConfiguration(void)   // Called when a Set Configuration command is received
{
  if( EZUSB_HIGHSPEED( ) )
  { // FX2 enumerated at high speed
    SYNCDELAY;                  // 
    EP6AUTOINLENH = 0x02;       // set AUTOIN commit length to 512 bytes
    SYNCDELAY;  
    EP6AUTOINLENL = 0x00;
    SYNCDELAY;                 // 
                      
    enum_high_speed = TRUE;
  }
  else
  { // FX2 enumerated at full speed
    SYNCDELAY;                   
    EP6AUTOINLENH = 0x00; 
    SYNCDELAY;                 
    EP6AUTOINLENL = 0x40;
    SYNCDELAY;                  
    enum_high_speed = FALSE;
  }

  Configuration = SETUPDAT[2];
  return(TRUE);            // Handled by user code
}

BOOL DR_GetConfiguration(void)   // Called when a Get Configuration command is received
{
   EP0BUF[0] = Configuration;
   EP0BCH = 0;
   EP0BCL = 1;
   return(TRUE);            // Handled by user code
}

BOOL DR_SetInterface(void)       // Called when a Set Interface command is received
{
   AlternateSetting = SETUPDAT[2];
   return(TRUE);            // Handled by user code
}

BOOL DR_GetInterface(void)       // Called when a Set Interface command is received
{
   EP0BUF[0] = AlternateSetting;
   EP0BCH = 0;
   EP0BCL = 1;
   return(TRUE);            // Handled by user code
}

BOOL DR_GetStatus(void)
{
   return(TRUE);
}

BOOL DR_ClearFeature(void)
{
   return(TRUE);
}

BOOL DR_SetFeature(void)
{
   return(TRUE);
}

BOOL DR_VendorCmnd(void)
{
  switch (SETUPDAT[1])
  {
    case CMD_GET_FW_VERSION:
	{
	  EP0BUF[0] = DSLOGICFW_VER_MAJOR;
	  EP0BUF[1] = DSLOGICFW_VER_MINOR;
	  EP0BCH = 0;
	  EP0BCL = 2;
	  break;
	}
    case CMD_GET_REVID_VERSION:
	{
	  EP0BUF[0] = REVID;
	  EP0BCH = 0;
	  EP0BCL = 1;
	  break;
	}
    case CMD_START:
	case CMD_SETTING:
	case CMD_CONTROL:
    {
      command = SETUPDAT[1];
      EP0BCL = 0;
	  break;
    }

    case CMD_CONFIG:
    {
	  EP0BCL = 0;
	  IOA |= 0x03;
	  cfg_init = TRUE;
	  break;
    }

    default:
      return(TRUE);
  }

  return(FALSE);
}

//-----------------------------------------------------------------------------
// USB Interrupt Handlers
//   The following functions are called by the USB interrupt jump table.
//-----------------------------------------------------------------------------

// Setup Data Available Interrupt Handler
void ISR_Sudav(void) interrupt 0
{
   GotSUD = TRUE;            // Set flag
   EZUSB_IRQ_CLEAR();
   USBIRQ = bmSUDAV;         // Clear SUDAV IRQ
}

// Setup Token Interrupt Handler
void ISR_Sutok(void) interrupt 0
{
   EZUSB_IRQ_CLEAR();
   USBIRQ = bmSUTOK;         // Clear SUTOK IRQ
}

void ISR_Sof(void) interrupt 0
{
   EZUSB_IRQ_CLEAR();
   USBIRQ = bmSOF;            // Clear SOF IRQ
}

void ISR_Ures(void) interrupt 0
{
   // whenever we get a USB reset, we should revert to full speed mode
   pConfigDscr = pFullSpeedConfigDscr;
   ((CONFIGDSCR xdata *) pConfigDscr)->type = CONFIG_DSCR;
   pOtherConfigDscr = pHighSpeedConfigDscr;
   ((CONFIGDSCR xdata *) pOtherConfigDscr)->type = OTHERSPEED_DSCR;

   EZUSB_IRQ_CLEAR();
   USBIRQ = bmURES;         // Clear URES IRQ
}

void ISR_Susp(void) interrupt 0
{
}

void ISR_Highspeed(void) interrupt 0
{
   if (EZUSB_HIGHSPEED())
   {
      pConfigDscr = pHighSpeedConfigDscr;
      ((CONFIGDSCR xdata *) pConfigDscr)->type = CONFIG_DSCR;
      pOtherConfigDscr = pFullSpeedConfigDscr;
      ((CONFIGDSCR xdata *) pOtherConfigDscr)->type = OTHERSPEED_DSCR;
   }

   EZUSB_IRQ_CLEAR();
   USBIRQ = bmHSGRANT;
}
void ISR_Ep0ack(void) interrupt 0
{
}
void ISR_Stub(void) interrupt 0
{
}
void ISR_Ep0in(void) interrupt 0
{
}
void ISR_Ep0out(void) interrupt 0
{
}
void ISR_Ep1in(void) interrupt 0
{
}
void ISR_Ep1out(void) interrupt 0
{
}
void ISR_Ep2inout(void) interrupt 0
{
}
void ISR_Ep4inout(void) interrupt 0
{
}
void ISR_Ep6inout(void) interrupt 0
{
}
void ISR_Ep8inout(void) interrupt 0
{
}
void ISR_Ibn(void) interrupt 0
{
}
void ISR_Ep0pingnak(void) interrupt 0
{
}
void ISR_Ep1pingnak(void) interrupt 0
{
}
void ISR_Ep2pingnak(void) interrupt 0
{
}
void ISR_Ep4pingnak(void) interrupt 0
{
}
void ISR_Ep6pingnak(void) interrupt 0
{
}
void ISR_Ep8pingnak(void) interrupt 0
{
}
void ISR_Errorlimit(void) interrupt 0
{
}
void ISR_Ep2piderror(void) interrupt 0
{
}
void ISR_Ep4piderror(void) interrupt 0
{
}
void ISR_Ep6piderror(void) interrupt 0
{
}
void ISR_Ep8piderror(void) interrupt 0
{
}
void ISR_Ep2pflag(void) interrupt 0
{
}
void ISR_Ep4pflag(void) interrupt 0
{
}
void ISR_Ep6pflag(void) interrupt 0
{
}
void ISR_Ep8pflag(void) interrupt 0
{
}
void ISR_Ep2eflag(void) interrupt 0
{
}
void ISR_Ep4eflag(void) interrupt 0
{
}
void ISR_Ep6eflag(void) interrupt 0
{
}
void ISR_Ep8eflag(void) interrupt 0
{
}
void ISR_Ep2fflag(void) interrupt 0
{
}
void ISR_Ep4fflag(void) interrupt 0
{
}
void ISR_Ep6fflag(void) interrupt 0
{
}
void ISR_Ep8fflag(void) interrupt 0
{
}
void ISR_GpifComplete(void) interrupt 0
{
}
void ISR_GpifWaveform(void) interrupt 0
{
}
