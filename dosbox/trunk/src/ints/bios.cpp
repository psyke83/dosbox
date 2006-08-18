/*
 *  Copyright (C) 2002-2006  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/* $Id: bios.cpp,v 1.63 2006-08-18 18:23:02 c2woody Exp $ */

#include "dosbox.h"
#include "mem.h"
#include "bios.h"
#include "regs.h"
#include "cpu.h"
#include "callback.h"
#include "inout.h"
#include "pic.h"
#include "joystick.h"
#include "mouse.h"
#include "setup.h"


/* if mem_systems 0 then size_extended is reported as the real size else 
 * zero is reported. ems and xms can increase or decrease the other_memsystems
 * counter using the BIOS_ZeroExtendedSize call */
static Bit16u size_extended;
static Bits other_memsystems=0;
void CMOS_SetRegister(Bitu regNr, Bit8u val); //For setting equipment word

static Bitu INT70_Handler(void) {
	/* Acknowledge irq with cmos */
	IO_Write(0x70,0xc);
	IO_Read(0x71);
	if (mem_readb(BIOS_WAIT_FLAG_ACTIVE)) {
		Bit32u count=mem_readd(BIOS_WAIT_FLAG_COUNT);
		if (count>997) {
			mem_writed(BIOS_WAIT_FLAG_COUNT,count-997);
		} else {
			mem_writed(BIOS_WAIT_FLAG_COUNT,0);
			PhysPt where=Real2Phys(mem_readd(BIOS_WAIT_FLAG_POINTER));
			mem_writeb(where,mem_readb(where)|0x80);
			mem_writeb(BIOS_WAIT_FLAG_ACTIVE,0);
			mem_writed(BIOS_WAIT_FLAG_POINTER,RealMake(0,BIOS_WAIT_FLAG_TEMP));
			IO_Write(0x70,0xb);
			IO_Write(0x71,IO_Read(0x71)&~0x40);
		}
	} 
	/* Signal EOI to both pics */
	IO_Write(0xa0,0x20);
	IO_Write(0x20,0x20);
	return 0;
}

CALLBACK_HandlerObject* tandy_DAC_callback[2];
static struct {
	Bit16u port;
	Bit8u irq;
	Bit8u dma;
} tandy_sb;

static bool Tandy_ProbeSBPort(Bit16u sbport) {
	IO_Write(sbport+0x6,1);
	IO_Write(sbport+0x6,0);
	while (!(IO_Read(sbport+0xe)&0x80)) ;
	if (IO_Read(sbport+0xa)==0xaa) return true;
	else return false;
}

static bool Tandy_InitializeSB() {
	/* see if soundblaster module available and at what port */
	if (Tandy_ProbeSBPort(0x220)) tandy_sb.port=0x220;
	else if (Tandy_ProbeSBPort(0x230)) tandy_sb.port=0x230;
	else if (Tandy_ProbeSBPort(0x210)) tandy_sb.port=0x210;
	else if (Tandy_ProbeSBPort(0x240)) tandy_sb.port=0x240;
	else if (Tandy_ProbeSBPort(0x250)) tandy_sb.port=0x250;
	else if (Tandy_ProbeSBPort(0x260)) tandy_sb.port=0x260;
	else {
		/* no soundblaster accessible, disable Tandy DAC */
		tandy_sb.port=0;
		return false;
	}

	/* try to detect IRQ setting */
	IO_Write(tandy_sb.port+0x4,0x80);
	Bit8u rval=IO_Read(tandy_sb.port+0x5);
	if (rval && (rval!=0xff)) {
		if (rval&1) tandy_sb.irq=0x02;
		else if (rval&2) tandy_sb.irq=0x05;
		else if (rval&4) tandy_sb.irq=0x07;
		else tandy_sb.irq=0x10;
	} else tandy_sb.irq=0x07;	/* assume irq=7 for older soundblaster settings */

	/* try to detect DMA setting */
	IO_Write(tandy_sb.port+0x4,0x81);
	rval=IO_Read(tandy_sb.port+0x5);
	if (rval && (rval!=0xff)) {
		if (rval&1) tandy_sb.dma=0x00;
		else if (rval&2) tandy_sb.dma=0x01;
		else tandy_sb.dma=0x03;
	} else tandy_sb.dma=0x01;	/* assume dma=1 for older soundblaster settings */

	return true;
}

/* check if Tandy DAC is still playing */
static bool Tandy_TransferInProgress(void) {
	if (real_readw(0x40,0xd0)) return true;			/* not yet done */
	if (real_readb(0x40,0xd4)==0xff) return false;	/* still in init-state */

	IO_Write(0x0c,0x00);
	Bit16u datalen=IO_ReadB(tandy_sb.dma*2+1);
	datalen|=(IO_ReadB(tandy_sb.dma*2+1)<<8);
	if (datalen==0xffff) return false;	/* no DMA transfer */
	else if ((datalen<0x10) && (real_readb(0x40,0xd4)==0x0f) && (real_readw(0x40,0xd2)==0x1c)) {
		/* stop already requested */
		return false;
	}
	return true;
}

static void Tandy_SetupTransfer(PhysPt bufpt,bool isplayback) {
	Bitu length=real_readw(0x40,0xd0);
	if (length==0) return;	/* nothing to do... */

	if (tandy_sb.port==0) return;

	/* revector IRQ-handler if necessary */
	RealPt current_irq=RealGetVec(tandy_sb.irq+8);
	if (current_irq!=tandy_DAC_callback[0]->Get_RealPointer()) {
		real_writed(0x40,0xd6,current_irq);
		RealSetVec(tandy_sb.irq+8,tandy_DAC_callback[0]->Get_RealPointer());
	}

	IO_Write(tandy_sb.port+0xc,0xd0);	/* stop DMA transfer */
	IO_Write(0x21,IO_Read(0x21)&(~(1<<tandy_sb.irq)));		/* unmask IRQ */
	IO_Write(tandy_sb.port+0xc,0xd1);	/* turn speaker on */
	IO_Write(0x0a,0x04|tandy_sb.dma);	/* mask DMA channel */
	IO_Write(0x0c,0x00);				/* clear DMA flipflop */
	if (isplayback) IO_Write(0x0b,0x48|tandy_sb.dma);
	else IO_Write(0x0b,0x44|tandy_sb.dma);
	/* set physical address of buffer */
	Bit8u bufpage=(Bit8u)((bufpt>>16)&0xff);
	IO_Write(tandy_sb.dma*2,(Bit8u)(bufpt&0xff));
	IO_Write(tandy_sb.dma*2,(Bit8u)((bufpt>>8)&0xff));
	switch (tandy_sb.dma) {
		case 0: IO_Write(0x87,bufpage); break;
		case 1: IO_Write(0x83,bufpage); break;
		case 2: IO_Write(0x81,bufpage); break;
		case 3: IO_Write(0x82,bufpage); break;
	}
	real_writeb(0x40,0xd4,bufpage);

	/* calculate transfer size (respects segment boundaries) */
	Bit32u tlength=length;
	if (tlength+(bufpt&0xffff)>0x10000) tlength=0x10000-(bufpt&0xffff);
	real_writew(0x40,0xd0,(Bit16u)(length-tlength));	/* remaining buffer length */
	tlength--;

	/* set transfer size */
	IO_Write(tandy_sb.dma*2+1,(Bit8u)(tlength&0xff));
	IO_Write(tandy_sb.dma*2+1,(Bit8u)((tlength>>8)&0xff));
	IO_Write(0x0a,tandy_sb.dma);	/* enable DMA channel */

	Bitu delay=real_readw(0x40,0xd2)&0xfff;
	/* set frequency */
	IO_Write(tandy_sb.port+0xc,0x40);
	IO_Write(tandy_sb.port+0xc,256-delay*100/358);
	/* set playback type to 8bit */
	if (isplayback) IO_Write(tandy_sb.port+0xc,0x14);
	else IO_Write(tandy_sb.port+0xc,0x24);
	/* set transfer size */
	IO_Write(tandy_sb.port+0xc,(Bit8u)(tlength&0xff));
	IO_Write(tandy_sb.port+0xc,(Bit8u)((tlength>>8)&0xff));

	if (!isplayback) {
		/* mark transfer as recording operation */
		real_writew(0x40,0xd2,delay|0x1000);
	}
}

static Bitu IRQ_TandyDAC(void) {
	if (real_readw(0x40,0xd0)) {	/* play/record next buffer */
		/* acknowledge IRQ */
		IO_Write(0x20,0x20);
		IO_Read(tandy_sb.port+0xe);

		/* buffer starts at the next page */
		Bit8u npage=real_readb(0x40,0xd4)+1;
		real_writeb(0x40,0xd4,npage);

		Bitu rb=real_readb(0x40,0xd3);
		if (rb&0x10) {
			/* start recording */
			real_writeb(0x40,0xd3,rb&0xef);
			Tandy_SetupTransfer(npage<<16,false);
		} else {
			/* start playback */
			Tandy_SetupTransfer(npage<<16,true);
		}
	} else {	/* playing/recording is finished */
		RealSetVec(tandy_sb.irq+8,real_readd(0x40,0xd6));

		/* turn off speaker and acknowledge soundblaster IRQ */
		IO_Write(tandy_sb.port+0xc,0xd3);
		IO_Read(tandy_sb.port+0xe);

		/* issue BIOS tandy sound device busy callout */
		SegSet16(cs, RealSeg(tandy_DAC_callback[1]->Get_RealPointer()));
		reg_ip = RealOff(tandy_DAC_callback[1]->Get_RealPointer());
	}
	return CBRET_NONE;
}

static void TandyDAC_Handler(Bit8u tfunction) {
	if (!tandy_sb.port) return;
	switch (tfunction) {
	case 0x81:	/* Tandy sound system check */
		reg_ax=0xc4;
		CALLBACK_SCF(Tandy_TransferInProgress());
		break;
	case 0x82:	/* Tandy sound system start recording */
	case 0x83:	/* Tandy sound system start playback */
		if (Tandy_TransferInProgress()) {
			/* cannot play yet as the last transfer isn't finished yet */
			reg_ah=0x00;
			CALLBACK_SCF(true);
			break;
		}
		/* store buffer length */
		real_writew(0x40,0xd0,reg_cx);
		/* store delay and volume */
		real_writew(0x40,0xd2,(reg_dx&0xfff)|((reg_al&7)<<13));
		Tandy_SetupTransfer(PhysMake(SegValue(es),reg_bx),reg_ah==0x83);
		reg_ah=0x00;
		CALLBACK_SCF(false);
		break;
	case 0x84:	/* Tandy sound system stop playing */
		reg_ah=0x00;

		/* setup for a small buffer with silence */
		real_writew(0x40,0xd0,0x0a);
		real_writew(0x40,0xd2,0x1c);
		Tandy_SetupTransfer(PhysMake(0xf000,0xa084),true);
		CALLBACK_SCF(false);
		break;
	case 0x85:	/* Tandy sound system reset */
		reg_ah=0x00;
		CALLBACK_SCF(false);
		break;
	}
}

static Bitu INT1A_Handler(void) {
	switch (reg_ah) {
	case 0x00:	/* Get System time */
		{
			Bit32u ticks=mem_readd(BIOS_TIMER);
			reg_al=0;		/* Midnight never passes :) */
			reg_cx=(Bit16u)(ticks >> 16);
			reg_dx=(Bit16u)(ticks & 0xffff);
			break;
		}
	case 0x01:	/* Set System time */
		mem_writed(BIOS_TIMER,(reg_cx<<16)|reg_dx);
		break;
	case 0x02:	/* GET REAL-TIME CLOCK TIME (AT,XT286,PS) */
		IO_Write(0x70,0x04);		//Hours
		reg_ch=IO_Read(0x71);
		IO_Write(0x70,0x02);		//Minutes
		reg_cl=IO_Read(0x71);
		IO_Write(0x70,0x00);		//Seconds
		reg_dh=IO_Read(0x71);
		reg_dl=0;			//Daylight saving disabled
		CALLBACK_SCF(false);
		break;
	case 0x04:	/* GET REAL-TIME ClOCK DATE  (AT,XT286,PS) */
		IO_Write(0x70,0x32);		//Centuries
		reg_ch=IO_Read(0x71);
		IO_Write(0x70,0x09);		//Years
		reg_cl=IO_Read(0x71);
		IO_Write(0x70,0x08);		//Months
		reg_dh=IO_Read(0x71);
		IO_Write(0x70,0x07);		//Days
		reg_dl=IO_Read(0x71);
		CALLBACK_SCF(false);
		break;
	case 0x80:	/* Pcjr Setup Sound Multiplexer */
		LOG(LOG_BIOS,LOG_ERROR)("INT1A:80:Setup tandy sound multiplexer to %d",reg_al);
		break;
	case 0x81:	/* Tandy sound system check */
	case 0x82:	/* Tandy sound system start recording */
	case 0x83:	/* Tandy sound system start playback */
	case 0x84:	/* Tandy sound system stop playing */
	case 0x85:	/* Tandy sound system reset */
		TandyDAC_Handler(reg_ah);
		break;
	case 0xb1:		/* PCI Bios Calls */
		LOG(LOG_BIOS,LOG_ERROR)("INT1A:PCI bios call %2X",reg_al);
		CALLBACK_SCF(true);
		break;
	default:
		LOG(LOG_BIOS,LOG_ERROR)("INT1A:Undefined call %2X",reg_ah);
	}
	return CBRET_NONE;
}	

static Bitu INT11_Handler(void) {
	reg_ax=mem_readw(BIOS_CONFIGURATION);
	return CBRET_NONE;
}

static Bitu INT8_Handler(void) {
	/* Increase the bios tick counter */
	mem_writed(BIOS_TIMER,mem_readd(BIOS_TIMER)+1);
	/* decrease floppy motor timer */
	Bit8u val = mem_readb(BIOS_DISK_MOTOR_TIMEOUT);
	if (val) mem_writeb(BIOS_DISK_MOTOR_TIMEOUT,val-1);
	/* and running drive */
	mem_writeb(BIOS_DRIVE_RUNNING,mem_readb(BIOS_DRIVE_RUNNING) & 0xF0);
	return CBRET_NONE;
};

static Bitu INT1C_Handler(void) {
	return CBRET_NONE;
};

static Bitu INT12_Handler(void) {
	reg_ax=mem_readw(BIOS_MEMORY_SIZE);
	return CBRET_NONE;
};

static Bitu INT17_Handler(void) {
	LOG(LOG_BIOS,LOG_NORMAL)("INT17:Function %X",reg_ah);
	switch(reg_ah) {
	case 0x00:		/* PRINTER: Write Character */
		reg_ah=1;	/* Report a timeout */
		break;
	case 0x01:		/* PRINTER: Initialize port */
		break;
	case 0x02:		/* PRINTER: Get Status */
		reg_ah=0;	
		break;
	case 0x20:		/* Some sort of printerdriver install check*/
		break;
	default:
		E_Exit("Unhandled INT 17 call %2X",reg_ah);
	};
	return CBRET_NONE;
}

static Bitu INT14_Handler(void)
{
	Bit16u port = real_readw(0x40,reg_dx*2); // DX is always port number
	if(reg_dx > 0x3 || port==0)	// no more than 4 serial ports
	{
		LOG_MSG("BIOS INT14: port %d does not exist.",reg_dx);
		return CBRET_NONE;
	}
	switch (reg_ah)
	{
	case 0x00:	/* Init port */
		// Parameters:
		// AL: port parameters
		// Return: 
		// AH: line status
		// AL: modem status
		{
			// set baud rate
			Bitu baudrate = 9600;
			Bit16u baudresult;
			Bitu rawbaud=reg_al>>5;
			
			if(rawbaud==0){ baudrate=110;}
			else if (rawbaud==1){ baudrate=150;}
			else if (rawbaud==2){ baudrate=300;}
			else if (rawbaud==3){ baudrate=600;}
			else if (rawbaud==4){ baudrate=1200;}
			else if (rawbaud==5){ baudrate=2400;}
			else if (rawbaud==6){ baudrate=4800;}
			else if (rawbaud==7){ baudrate=9600;}

			baudresult = (Bit16u)(115200 / baudrate);

			IO_WriteB(port+3, 0x80);	// enable divider access
			IO_WriteB(port,(Bit8u)baudresult&0xff);
			IO_WriteB(port+1,(Bit8u)(baudresult>>8));

			// set line parameters, disable divider access
			IO_WriteB(port+3, reg_al&0x1F);//LCR
			
			// disable interrupts
			IO_WriteB(port+1, 0);
			IO_ReadB(port+2);
			// put RTS and DTR on
			IO_WriteB(port+4,0x3);

			// get result
			reg_ah=IO_ReadB(port+5);
			reg_al=IO_ReadB(port+6);
		}
		break;
	case 0x01:	/* Write character */
		// Parameters:
		// AL: character
		// Return: 
		// AH: line status
		// AL: modem status
		{
			if((IO_ReadB(port+5)&&0x20)==0)
			{
				// TODO: should wait until they become empty->timeout
				LOG_MSG("BIOS INT14: port %d: transmit register not empty.",reg_dx);
				reg_ah = IO_ReadB(port+5)|0x80;
				return CBRET_NONE;
			}
			// transmit it
			IO_WriteB(port,reg_al);
			
			if((IO_ReadB(port+5)&&0x60)==0)
			{
				// TODO: should wait until they become empty->timeout
				LOG_MSG("BIOS INT14: port %d: transmit register not empty after write.",reg_dx);
				reg_ah = IO_ReadB(port+5)|0x80;
				return CBRET_NONE;
			}
			
			// get result
			reg_ah=IO_ReadB(port+5);
			reg_al=IO_ReadB(port+6);
		}
		break;
	
	case 0x02:	/* Read character */
		{
			if((IO_ReadB(port+5)&0x1)!=0)
			{
				reg_al=IO_ReadB(port);
			}
			else
			{
				// TODO: should wait until timeout
				LOG_MSG("BIOS INT14: port %d: nothing received.",reg_dx);
				reg_ah = IO_ReadB(port+5)|0x80;
				return CBRET_NONE;
			}
			reg_ah=IO_ReadB(port+5);
		}
		break;
	case 0x03: // get status
		{
			reg_ah=IO_ReadB(port+5);
			//LOG_MSG("status reg_ah: %x",reg_ah);
			reg_al=IO_ReadB(port+6);
		}
		break;
	case 0x04:	// extended initialisation
		// Parameters:
		// AL: break
		// BH: parity
		// BL: stopbit
		// CH: word length
		// CL: baudrate
		{
			Bit8u lcr = 0;
			
			// baud rate
			Bitu baudrate = 9600;
			Bit16u baudresult;
			Bitu rawbaud=reg_cl;
			
			if(rawbaud==0){ baudrate=110;}
			else if (rawbaud==1){ baudrate=150;}
			else if (rawbaud==2){ baudrate=300;}
			else if (rawbaud==3){ baudrate=600;}
			else if (rawbaud==4){ baudrate=1200;}
			else if (rawbaud==5){ baudrate=2400;}
			else if (rawbaud==6){ baudrate=4800;}
			else if (rawbaud==7){ baudrate=9600;}
			else if (rawbaud==8){ baudrate=19200;}

			baudresult = (Bit16u)(115200 / baudrate);

			IO_WriteB(port+3, 0x80);	// enable divider access
			IO_WriteB(port,(Bit8u)baudresult&0xff);
			IO_WriteB(port+1,(Bit8u)(baudresult>>8));
			
			// line configuration
			// break
			if(reg_al!=0) lcr=0x40;
			// parity
			if(reg_bh!=0)
			{
				if(reg_bh==1)lcr|=0x8;// odd
				else if(reg_bh==2)lcr|=0x18;// even
				else if(reg_bh==3)lcr|=0x28;// mark
				else if(reg_bh==4)lcr|=0x38;// mark
			}
			// stopbit
			if(reg_bl!=0)
			{
				lcr|=0x4;
			}
			// data length
			lcr|=(reg_ch&0x3);
			IO_WriteB(port+3,lcr);

			reg_ah=IO_ReadB(port+5);
			reg_al=IO_ReadB(port+6);
		}
		break;
	case 0x05:	// modem control
		{
			if(reg_al==0)	// read MCR
			{
				reg_bl=IO_ReadB(port+4);
			}
			else if(reg_al==1)	// write MCR
			{
				IO_WriteB(port+4,reg_bl);
			}
			else LOG_MSG("BIOS INT14: port %d, function 5: invalid subfunction.",reg_dx);
			reg_ah=IO_ReadB(port+5);
			reg_al=IO_ReadB(port+6);
		}
		break;
	default:
		LOG_MSG("Unhandled INT 14 call %2X",reg_ah);
		
	}
	return CBRET_NONE;
}

static Bitu INT15_Handler(void) {
	static Bitu biosConfigSeg=0;
	switch (reg_ah) {
	case 0x06:
		LOG(LOG_BIOS,LOG_NORMAL)("INT15 Unkown Function 6");
		break;
	case 0xC0:	/* Get Configuration*/
		{
			if (biosConfigSeg==0) biosConfigSeg = DOS_GetMemory(1); //We have 16 bytes
			PhysPt data	= PhysMake(biosConfigSeg,0);
			mem_writew(data,8);						// 8 Bytes following
			if (IS_TANDY_ARCH) {
				if (machine==MCH_TANDY) {
					// Model ID (Tandy)
					mem_writeb(data+2,0xFF);
				} else {
					// Model ID (PCJR)
					mem_writeb(data+2,0xFD);
				}
				mem_writeb(data+3,0x0A);					// Submodel ID
				mem_writeb(data+4,0x10);					// Bios Revision
				/* Tandy doesn't have a 2nd PIC, left as is for now */
				mem_writeb(data+5,(1<<6)|(1<<5)|(1<<4));	// Feature Byte 1
			} else {
				mem_writeb(data+2,0xFC);					// Model ID (PC)
				mem_writeb(data+3,0x00);					// Submodel ID
				mem_writeb(data+4,0x01);					// Bios Revision
				mem_writeb(data+5,(1<<6)|(1<<5)|(1<<4));	// Feature Byte 1
			}
			mem_writeb(data+6,(1<<6));				// Feature Byte 2
			mem_writeb(data+7,0);					// Feature Byte 3
			mem_writeb(data+8,0);					// Feature Byte 4
			mem_writeb(data+9,0);					// Feature Byte 5
			CPU_SetSegGeneral(es,biosConfigSeg);
			reg_bx = 0;
			reg_ah = 0;
			CALLBACK_SCF(false);
		}; break;
	case 0x4f:	/* BIOS - Keyboard intercept */
		/* Carry should be set but let's just set it just in case */
		CALLBACK_SCF(true);
		break;
	case 0x83:	/* BIOS - SET EVENT WAIT INTERVAL */
		{
			if(reg_al == 0x01) { /* Cancel it */
				mem_writeb(BIOS_WAIT_FLAG_ACTIVE,0);
				IO_Write(0x70,0xb);
				IO_Write(0x71,IO_Read(0x71)&~0x40);
				CALLBACK_SCF(false);
				break;
			}
			if (mem_readb(BIOS_WAIT_FLAG_ACTIVE)) {
				reg_ah=0x80;
				CALLBACK_SCF(true);
				break;
			}
			Bit32u count=(reg_cx<<16)|reg_dx;
			mem_writed(BIOS_WAIT_FLAG_POINTER,RealMake(SegValue(es),reg_bx));
			mem_writed(BIOS_WAIT_FLAG_COUNT,count);
			mem_writeb(BIOS_WAIT_FLAG_ACTIVE,1);
			/* Reprogram RTC to start */
			IO_Write(0x70,0xb);
			IO_Write(0x71,IO_Read(0x71)|0x40);
			CALLBACK_SCF(false);
		}
		break;
	case 0x84:	/* BIOS - JOYSTICK SUPPORT (XT after 11/8/82,AT,XT286,PS) */
		if (reg_dx == 0x0000) {
			// Get Joystick button status
			if (JOYSTICK_IsEnabled(0) || JOYSTICK_IsEnabled(1)) {
				reg_al = IO_ReadB(0x201)&0xf0;
				CALLBACK_SCF(false);
			} else {
				// dos values
				reg_ax = 0x00f0; reg_dx = 0x0201;
				CALLBACK_SCF(true);
			}
		} else if (reg_dx == 0x0001) {
			if (JOYSTICK_IsEnabled(0)) {
				reg_ax = (Bit16u)(JOYSTICK_GetMove_X(0)*127+128);
				reg_bx = (Bit16u)(JOYSTICK_GetMove_Y(0)*127+128);
				if(JOYSTICK_IsEnabled(1)) {
					reg_cx = (Bit16u)(JOYSTICK_GetMove_X(1)*127+128);
					reg_dx = (Bit16u)(JOYSTICK_GetMove_Y(1)*127+128);
				}
				else {
					reg_cx = reg_dx = 0;
				}
				CALLBACK_SCF(false);
			} else if (JOYSTICK_IsEnabled(1)) {
				reg_ax = reg_bx = 0;
				reg_cx = (Bit16u)(JOYSTICK_GetMove_X(1)*127+128);
				reg_dx = (Bit16u)(JOYSTICK_GetMove_Y(1)*127+128);
				CALLBACK_SCF(false);
			} else {			
				reg_ax = reg_bx = reg_cx = reg_dx = 0;
				CALLBACK_SCF(true);
			}
		} else {
			LOG(LOG_BIOS,LOG_ERROR)("INT15:84:Unknown Bios Joystick functionality.");
		}
		break;
	case 0x86:	/* BIOS - WAIT (AT,PS) */
		{
			if (mem_readb(BIOS_WAIT_FLAG_ACTIVE)) {
				reg_ah=0x83;
				CALLBACK_SCF(true);
				break;
			}
			Bit32u count=(reg_cx<<16)|reg_dx;
			mem_writed(BIOS_WAIT_FLAG_POINTER,RealMake(0,BIOS_WAIT_FLAG_TEMP));
			mem_writed(BIOS_WAIT_FLAG_COUNT,count);
			mem_writeb(BIOS_WAIT_FLAG_ACTIVE,1);
			/* Reprogram RTC to start */
			IO_Write(0x70,0xb);
			IO_Write(0x71,IO_Read(0x71)|0x40);
			while (mem_readd(BIOS_WAIT_FLAG_COUNT)) {
				CALLBACK_Idle();
			}
			CALLBACK_SCF(false);
		}
	case 0x87:	/* Copy extended memory */
		{
			bool enabled = MEM_A20_Enabled();
			MEM_A20_Enable(true);
			Bitu   bytes	= reg_cx * 2;
			PhysPt data		= SegPhys(es)+reg_si;
			PhysPt source	= mem_readd(data+0x12) & 0x00FFFFFF + (mem_readb(data+0x16)<<24);
			PhysPt dest		= mem_readd(data+0x1A) & 0x00FFFFFF + (mem_readb(data+0x1E)<<24);
			MEM_BlockCopy(dest,source,bytes);
			reg_ax = 0x00;
			MEM_A20_Enable(enabled);
			CALLBACK_SCF(false);
			break;
		}	
	case 0x88:	/* SYSTEM - GET EXTENDED MEMORY SIZE (286+) */
		reg_ax=other_memsystems?0:size_extended;
		LOG(LOG_BIOS,LOG_NORMAL)("INT15:Function 0x88 Remaining %04X kb",reg_ax);
		CALLBACK_SCF(false);
		break;
	case 0x89:	/* SYSTEM - SWITCH TO PROTECTED MODE */
		{
			IO_Write(0x20,0x10);IO_Write(0x21,reg_bh);IO_Write(0x21,0);
			IO_Write(0xA0,0x10);IO_Write(0xA1,reg_bl);IO_Write(0xA1,0);
			MEM_A20_Enable(true);
			PhysPt table=SegPhys(es)+reg_si;
			CPU_LGDT(mem_readw(table+0x8),mem_readd(table+0x8+0x2) & 0xFFFFFF);
			CPU_LIDT(mem_readw(table+0x10),mem_readd(table+0x10+0x2) & 0xFFFFFF);
			CPU_SET_CRX(0,CPU_GET_CRX(0)|1);
			CPU_SetSegGeneral(ds,0x18);
			CPU_SetSegGeneral(es,0x20);
			CPU_SetSegGeneral(ss,0x28);
			reg_sp+=6;			//Clear stack of interrupt frame
			CPU_SetFlags(0,FMASK_ALL);
			reg_ax=0;
			CPU_JMP(false,0x30,reg_cx,0);
		}
		break;
	case 0x90:	/* OS HOOK - DEVICE BUSY */
		CALLBACK_SCF(false);
		reg_ah=0;
		break;
	case 0x91:	/* OS HOOK - DEVICE POST */
		CALLBACK_SCF(false);
		reg_ah=0;
		break;
	case 0xc2:	/* BIOS PS2 Pointing Device Support */
		switch (reg_al) {
		case 0x00:		// enable/disable
			if (reg_bh==0) {	// disable
				Mouse_SetPS2State(false);
				reg_ah=0;
				CALLBACK_SCF(false);
			} else if (reg_bh==0x01) {	//enable
				if (!Mouse_SetPS2State(true)) {
					reg_ah=5;
					CALLBACK_SCF(true);
					break;
				}
				reg_ah=0;
				CALLBACK_SCF(false);
			} else {
				CALLBACK_SCF(true);
				reg_ah=1;
			}
			break;
		case 0x01:		// reset
			reg_bx=0x00aa;	// mouse
			// fall through
		case 0x05:		// initialize
			Mouse_SetPS2State(false);
			CALLBACK_SCF(false);
			reg_ah=0;
			break;
		case 0x02:		// set sampling rate
		case 0x03:		// set resolution
			CALLBACK_SCF(false);
			reg_ah=0;
			break;
		case 0x04:		// get type
			reg_bh=0;	// ID
			CALLBACK_SCF(false);
			reg_ah=0;
			break;
		case 0x06:		// extended commands
			if ((reg_bh==0x01) || (reg_bh==0x02)) {
				CALLBACK_SCF(false); 
				reg_ah=0;
			} else {
				CALLBACK_SCF(true);
				reg_ah=1;
			}
			break;
		case 0x07:		// set callback
			Mouse_ChangePS2Callback(SegValue(es),reg_bx);
			CALLBACK_SCF(false);
			reg_ah=0;
			break;
		default:
			CALLBACK_SCF(true);
			reg_ah=1;
			break;
		}
		break;
	case 0xc3:      /* set carry flag so BorlandRTM doesn't assume a VECTRA/PS2 */
		reg_ah=0x86;
		CALLBACK_SCF(true);
		break;
	case 0xc4:	/* BIOS POS Programm option Select */
		LOG(LOG_BIOS,LOG_NORMAL)("INT15:Function %X called, bios mouse not supported",reg_ah);
		CALLBACK_SCF(true);
		break;
	default:
		LOG(LOG_BIOS,LOG_ERROR)("INT15:Unknown call %4X",reg_ax);
		reg_ah=0x86;
		CALLBACK_SCF(true);
		if ((machine==MCH_VGA) || (machine==MCH_CGA)) {
			/* relict from comparisons, as int15 exits with a retf2 instead of an iret */
			CALLBACK_SZF(false);
		}
	}
	return CBRET_NONE;
}

void BIOS_ZeroExtendedSize(bool in) {
	if(in) other_memsystems++; 
	else other_memsystems--;
	if(other_memsystems < 0) other_memsystems=0;
}

#define RAM_REFRESH_DELAY 16.7f

static void RAMRefresh_Event(Bitu val) {
	PIC_ActivateIRQ(5);
	PIC_AddEvent(RAMRefresh_Event,RAM_REFRESH_DELAY);
}

void BIOS_SetupKeyboard(void);
void BIOS_SetupDisks(void);

class BIOS:public Module_base{
private:
	CALLBACK_HandlerObject callback[10];
public:
	BIOS(Section* configuration):Module_base(configuration){
		/* tandy DAC can be requested in tandy_sound.cpp by initializing this field */
		bool use_tandyDAC=(real_readb(0x40,0xd4)==0xff);

		/* Clear the Bios Data Area (0x400-0x5ff, 0x600- is accounted to DOS) */
		for (Bit16u i=0;i<0x200;i++) real_writeb(0x40,i,0);

		/* Setup all the interrupt handlers the bios controls */
		/* INT 8 Clock IRQ Handler */
		callback[0].Install(INT8_Handler,CB_IRQ0,"Int 8 Clock");
		callback[0].Set_RealVec(0x8);
		// pseudocode for CB_IRQ0:
		//	callback INT8_Handler
		//	push ax,dx,ds
		//	int 0x1c
		//	cli
		//	pop ds,dx
		//	mov al, 0x20
		//	out 0x20, al
		//	pop ax
		//	iret

		mem_writed(BIOS_TIMER,0);			//Calculate the correct time

		/* INT 11 Get equipment list */
		callback[1].Install(&INT11_Handler,CB_IRET,"Int 11 Equipment");
		callback[1].Set_RealVec(0x11);

		/* INT 12 Memory Size default at 640 kb */
		callback[2].Install(&INT12_Handler,CB_IRET,"Int 12 Memory");
		callback[2].Set_RealVec(0x12);
		if (IS_TANDY_ARCH) {
			/* reduce reported memory size for the Tandy (32k graphics memory
			   at the end of the conventional 640k) */
			if (machine==MCH_TANDY) mem_writew(BIOS_MEMORY_SIZE,608);
			else mem_writew(BIOS_MEMORY_SIZE,640);
			mem_writew(BIOS_TRUE_MEMORY_SIZE,640);
		} else mem_writew(BIOS_MEMORY_SIZE,640);
		
		/* INT 13 Bios Disk Support */
		BIOS_SetupDisks();

		/* INT 14 Serial Ports */
		callback[3].Install(&INT14_Handler,CB_IRET,"Int 14 COM-port");
		callback[3].Set_RealVec(0x14);

		/* INT 15 Misc Calls */
		callback[4].Install(&INT15_Handler,CB_IRET,"Int 15 Bios");
		callback[4].Set_RealVec(0x15);

		/* INT 16 Keyboard handled in another file */
		BIOS_SetupKeyboard();

		/* INT 17 Printer Routines */
		callback[5].Install(&INT17_Handler,CB_IRET,"Int 17 Printer");
		callback[5].Set_RealVec(0x17);

		/* INT 1A TIME and some other functions */
		callback[6].Install(&INT1A_Handler,CB_IRET_STI,"Int 1a Time");
		callback[6].Set_RealVec(0x1A);

		/* INT 1C System Timer tick called from INT 8 */
		callback[7].Install(&INT1C_Handler,CB_IRET,"Int 1c Timer");
		callback[7].Set_RealVec(0x1C);
		
		/* IRQ 8 RTC Handler */
		callback[8].Install(&INT70_Handler,CB_IRET,"Int 70 RTC");
		callback[8].Set_RealVec(0x70);

		/* Irq 9 rerouted to irq 2 */
		callback[9].Install(NULL,CB_IRQ9,"irq 9 bios");
		callback[9].Set_RealVec(0x71);

		/* Irq 2 */
		RealPt irq2pt=RealMake(0xf000,0xff55);	/* Ghost busters 2 mt32 mode */
		Bitu call_irq2=CALLBACK_Allocate();	
		CALLBACK_Setup(call_irq2,NULL,CB_IRET_EOI_PIC1,Real2Phys(irq2pt),"irq 2 bios");
		RealSetVec(0x0a,irq2pt);

		/* Some hardcoded vectors */
		phys_writeb(0xfff53,0xcf);	/* bios default interrupt vector location */
		phys_writeb(0xfe987,0xea);	/* original IRQ1 location (Defender booter) */
		phys_writed(0xfe988,RealGetVec(0x09));

		if (machine==MCH_TANDY) phys_writeb(0xffffe,0xff)	;	/* Tandy model */
		else if (machine==MCH_PCJR) phys_writeb(0xffffe,0xfd);	/* PCJr model */
		else phys_writeb(0xffffe,0xfc);	/* PC */

		if (use_tandyDAC) {
			/* tandy DAC sound requested, see if soundblaster device is available */
			if (Tandy_InitializeSB()) {
				real_writew(0x40,0xd0,0x0000);
				real_writew(0x40,0xd2,0x0000);
				real_writeb(0x40,0xd4,0xff);	/* tandy DAC init value */
				real_writed(0x40,0xd6,0x00000000);
				/* install the DAC callback handler */
				tandy_DAC_callback[0]=new CALLBACK_HandlerObject();
				tandy_DAC_callback[1]=new CALLBACK_HandlerObject();
				tandy_DAC_callback[0]->Install(&IRQ_TandyDAC,CB_IRET,"Tandy DAC IRQ");
				tandy_DAC_callback[1]->Install(NULL,CB_TDE_IRET,"Tandy DAC end transfer");
				// pseudocode for CB_TDE_IRET:
				//	push ax
				//	mov ax, 0x91fb
				//	int 15
				//	cli
				//	mov al, 0x20
				//	out 0x20, al
				//	pop ax
				//	iret

				RealPt current_irq=RealGetVec(tandy_sb.irq+8);
				real_writed(0x40,0xd6,current_irq);
				for (Bitu i=0; i<0x10; i++) phys_writeb(PhysMake(0xf000,0xa084+i),0x80);
			} else real_writeb(0x40,0xd4,0x00);
		}
	
		/* Setup some stuff in 0x40 bios segment */
		/* detect parallel ports */
		Bit8u DEFAULTPORTTIMEOUT = 10;	// 10 whatevers
		Bitu ppindex=0; // number of lpt ports
		if ((IO_Read(0x378)!=0xff)|(IO_Read(0x379)!=0xff)) {
			// this is our LPT1
			mem_writew(BIOS_ADDRESS_LPT1,0x378);
			mem_writeb(BIOS_LPT1_TIMEOUT,DEFAULTPORTTIMEOUT);
			ppindex++;
			if((IO_Read(0x278)!=0xff)|(IO_Read(0x279)!=0xff)) {
				// this is our LPT2
				mem_writew(BIOS_ADDRESS_LPT2,0x278);
				mem_writeb(BIOS_LPT2_TIMEOUT,DEFAULTPORTTIMEOUT);
				ppindex++;
				if((IO_Read(0x3bc)!=0xff)|(IO_Read(0x3be)!=0xff)) {
					// this is our LPT3
					mem_writew(BIOS_ADDRESS_LPT3,0x3bc);
					mem_writeb(BIOS_LPT3_TIMEOUT,DEFAULTPORTTIMEOUT);
					ppindex++;
				}
			} else if((IO_Read(0x3bc)!=0xff)|(IO_Read(0x3be)!=0xff)) {
				// this is our LPT2
				mem_writew(BIOS_ADDRESS_LPT2,0x3bc);
				mem_writeb(BIOS_LPT2_TIMEOUT,DEFAULTPORTTIMEOUT);
				ppindex++;
			}
		} else if((IO_Read(0x3bc)!=0xff)|(IO_Read(0x3be)!=0xff)) {
			// this is our LPT1
			mem_writew(BIOS_ADDRESS_LPT1,0x3bc);
			mem_writeb(BIOS_LPT1_TIMEOUT,DEFAULTPORTTIMEOUT);
			ppindex++;
			if((IO_Read(0x278)!=0xff)|(IO_Read(0x279)!=0xff)) {
				// this is our LPT2
				mem_writew(BIOS_ADDRESS_LPT2,0x278);
				mem_writeb(BIOS_LPT2_TIMEOUT,DEFAULTPORTTIMEOUT);
				ppindex++;
			}
		} else if((IO_Read(0x278)!=0xff)|(IO_Read(0x279)!=0xff)) {
			// this is our LPT1
			mem_writew(BIOS_ADDRESS_LPT1,0x278);
			mem_writeb(BIOS_LPT1_TIMEOUT,DEFAULTPORTTIMEOUT);
			ppindex++;
		}

		/* Setup equipment list */
		// look http://www.bioscentral.com/misc/bda.htm
		
		//Bitu config=0x4400;	//1 Floppy, 2 serial and 1 parrallel 
		Bitu config = 0x0;
		
		// set number of parallel ports
		// if(ppindex == 0) config |= 0x8000; // looks like 0 ports are not specified
		//else if(ppindex == 1) config |= 0x0000;
		if(ppindex == 2) config |= 0x4000;
		else config |= 0xc000;	// 3 ports
#if (C_FPU)
		//FPU
		config|=0x2;
#endif
		switch (machine) {
		case MCH_HERC:
			//Startup monochrome
			config|=0x30;
			break;
		case MCH_VGA:
		case MCH_CGA:
		case TANDY_ARCH_CASE:
			//Startup 80x25 color
			config|=0x20;
			break;
		default:
			//EGA VGA
			config|=0;
			break;
		}
		// PS2 mouse
		config |= 0x04;
		// Gameport
		config |= 0x1000;
		mem_writew(BIOS_CONFIGURATION,config);
		CMOS_SetRegister(0x14,config); //Should be updated on changes
		/* Setup extended memory size */
		IO_Write(0x70,0x30);
		size_extended=IO_Read(0x71);
		IO_Write(0x70,0x31);
		size_extended|=(IO_Read(0x71) << 8);

		if (machine==MCH_PCJR) PIC_AddEvent(RAMRefresh_Event,RAM_REFRESH_DELAY);
	}
	~BIOS(){
		/* abort DAC playing */
		if (tandy_sb.port) {
			IO_Write(tandy_sb.port+0xc,0xd3);
			IO_Write(tandy_sb.port+0xc,0xd0);
		}
		real_writeb(0x40,0xd4,0x00);
		if (tandy_DAC_callback[0]) {
			Bit32u orig_vector=real_readd(0x40,0xd6);
			if (orig_vector==tandy_DAC_callback[0]->Get_RealPointer()) {
				/* set IRQ vector to old value */
				RealSetVec(tandy_sb.irq+8,real_readd(0x40,0xd6));
				real_writed(0x40,0xd6,0x00000000);
			}
			delete tandy_DAC_callback[0];
			delete tandy_DAC_callback[1];
			tandy_DAC_callback[0]=NULL;
			tandy_DAC_callback[1]=NULL;
		}
	}
};

// set com port data in bios data area
// parameter: array of 4 com port base addresses, 0 = none
void BIOS_SetComPorts(Bit16u baseaddr[]) {
	Bit8u DEFAULTPORTTIMEOUT = 10;	// 10 whatevers
	Bit16u portcount=0;
	Bit16u equipmentword;
	for(Bitu i = 0; i < 4; i++) {
		if(baseaddr[i]!=0) portcount++;
		if(i==0) {	// com1
			mem_writew(BIOS_BASE_ADDRESS_COM1,baseaddr[i]);
			mem_writeb(BIOS_COM1_TIMEOUT,DEFAULTPORTTIMEOUT);
		} else if(i==1) {
			mem_writew(BIOS_BASE_ADDRESS_COM2,baseaddr[i]);
			mem_writeb(BIOS_COM2_TIMEOUT,DEFAULTPORTTIMEOUT);
		} else if(i==2) {
			mem_writew(BIOS_BASE_ADDRESS_COM3,baseaddr[i]);
			mem_writeb(BIOS_COM3_TIMEOUT,DEFAULTPORTTIMEOUT);
		} else {
			mem_writew(BIOS_BASE_ADDRESS_COM4,baseaddr[i]);
			mem_writeb(BIOS_COM4_TIMEOUT,DEFAULTPORTTIMEOUT);
		}
	}
	// set equipment word
	equipmentword = mem_readw(BIOS_CONFIGURATION);
	equipmentword &= (~0x0E00);
	equipmentword |= (portcount << 9);
	mem_writew(BIOS_CONFIGURATION,equipmentword);
	CMOS_SetRegister(0x14,equipmentword); //Should be updated on changes
}


static BIOS* test;

void BIOS_Destroy(Section* sec){
	delete test;
}

void BIOS_Init(Section* sec) {
	test = new BIOS(sec);
	sec->AddDestroyFunction(&BIOS_Destroy,false);
}
