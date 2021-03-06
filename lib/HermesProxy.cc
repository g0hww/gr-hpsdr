/* -*- c++ -*- */
/* 
 * Copyright 2013-2015 Tom McDermott, N5EG
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

//
// Hermes Proxy
//
// Encapsulates the Hermes module for access/control by GNU Radio.
// Used by the HermesNB (Narrow Band) and HermesWB (Wide Band) modules
// that provide Hermes sink and source to GNU Radio.
//
// Data+Control --> 2 x USB-like 512-byte frames.
// 2 x USB-like frames --> UDP IP-packet.
// UDP IP packet --> Ethernet frame --> Send to Hermes.
// Reverse happens for data from Hermes. 
// See the HPSDR documentation for USB and Ethernet frame formats.
//
// Uses the Metis Ethernet interface module to send/receive Ethernet
// frames to/from Hermes.
//
// HermesNB uses this proxy to convert raw data and control flags
// and send/receive them to Hermes.
//
// Version:  December 15, 2012
// Updates:   Make Clock Source and AlexControl programmable from GUI
//           July 10, 2013 - update for GRC 3.7
//	     December 4, 2013 - additional parameters in constructor	
//           March 13, 2014 - flip transmit I and Q symbols, due to FPGA
//           reversing them. Set TxDrive default to 0 (rather than 255).

#include <gnuradio/io_signature.h>
#include "HermesProxy.h"
#include "metis.h"
#include <stdio.h>
#include <cstring>


HermesProxy::HermesProxy(int RxFreq0, int RxFreq1, int TxFreq, bool RxPre,
			 int PTTModeSel, bool PTTTxMute, bool PTTRxMute,
			 unsigned char TxDr, int RxSmp, const char* Intfc, 
			 const char * ClkS, int AlexRA, int AlexTA,
			 int AlexHPF, int AlexLPF, int Verb, int NumRx,
			 const char* MACAddr)	// constructor
{


	//pthread_mutex_init (&mutexRPG, NULL);
	//pthread_mutex_init (&mutexGPT, NULL);
	//
	// Notes in case needed...
	//
	//  pthread_mutex_lock(&mutex) - acquire a lock on the specified mutex variable. If the
	// mutex is already locked by another thread, this call will block the calling thread
	// until the mutex is unlocked.

	//  pthread_mutex_unlock(&mutex) - unlock a mutex variable. An error is returned if mutex
	// is already unlocked or owned by another thread.

	//  pthread_mutex_trylock(&mutex) - attempt to lock a mutex or will return error code if
	// busy. Useful for preventing deadlock conditions. RETURN VALUES
	// On success, pthread_mutex_trylock() returns 0. On error, one of the following
	// values is returned:
	//	EBUSY    The mutex is already locked.
	//	EINVAL   mutex is not an initialized mutex.
	//	EFAULT   mutex is an invalid pointer.


	RxSampleRate = RxSmp;
	strcpy(interface, Intfc);	// Ethernet interface to use (defaults to eth0)
	NumReceivers = NumRx;

	unsigned int cs;		// Convert ClockSource strings to unsigned, then intitalize
	sscanf(ClkS, "%x", &cs);
	ClockSource = (cs & 0xFC);

//	Initialize the Alex control registers.

	AlexRxAnt = AlexRA;		// Select Alex Receive Antenna or from T/R relay
	AlexTxAnt = AlexTA;		// Select Alex Tx Antenna
	AlexRxHPF = AlexHPF;		// Select Alex Receive High Pass Filter
	AlexTxLPF = AlexLPF;		// Select Alex Transmit Low Pass Filter

	Verbose = Verb;			// Turn Verbose mode on/off

        for (int i=0; i<18; i++)
	  mactarget[i] = toupper(MACAddr[i]);	// Copy the requested MAC target address

	Receive0Frequency = (unsigned)RxFreq0;
	Receive1Frequency = (unsigned)RxFreq1; 
	TransmitFrequency = (unsigned)TxFreq;		// initialize frequencies
	TxDrive = TxDr;		// default to (almost) off
	PTTMode = PTTModeSel;
	RxPreamp = RxPre;
	PTTOffMutesTx = PTTTxMute;   // PTT Off mutes the transmitter
	PTTOnMutesRx = PTTRxMute;	// PTT On mutes receiver

	ADCdither = false;
	ADCrandom = false;
	RxAtten = 0;		// Hermes V2.0
	Duplex = true;		// Allows TxF to program separately from RxF

	TxStop = false;

	RxWriteCounter = 0;	//
	RxReadCounter = 0;	// These control the Rx buffers to Gnuradio
	RxWriteFill = 0;	//

	TxWriteCounter = 0;	//
 	TxReadCounter = 0;	// These control the Tx buffers to Hermes
	TxControlCycler = 0;	//
	TxFrameIdleCount = 0;	//

	LostRxBufCount = 0;	//
	TotalRxBufCount = 0;	//
	LostTxBufCount = 0;	//
	TotalTxBufCount = 0;	// diagnostics
	CorruptRxCount = 0;	//
	LostEthernetRx = 0;	//
	CurrentEthSeqNum = 0;	//

	
	// allocate the receiver buffers
	for(int i=0; i<NUMRXIQBUFS; i++)
		RxIQBuf[i] = new float[RXBUFSIZE];

	// allocate the transmit buffers
	for(int i=0; i<NUMTXBUFS; i++)
		TxBuf[i] = new unsigned char[TXBUFSIZE];

	metis_discover((const char *)(interface));

//
// If there is no specified MAC address (i.e. wildcard, or anything less than 17 
// charracters, then just grab the first Hermes/Metis that
// responds to discovery. If there is a specific MAC address specified, then wait
// until it appears in the Metis cards table, and set the metis table index to match.
// The string is HH:HH:HH:HH:HH:HH\0 formated, where HH is a 2-digital Hexidecimal number
// uppercase, example:    04:7F:3D:0F:28:5A
//

	metis_entry = 0;
	if (strlen(mactarget) != 17)			// Not a fully-qualified MAC address, default to first MAC found
	{
	  while (metis_found() == 0)
		;					// wait until Hermes responds with first discovered MAC
	}
	else						// Search the table for the entry matching requested MAC address
	{
	  bool found = false;
	  while(!found)					// Search for MAC address in the metis_table until the cows come home
	    for(int i=0; i<metis_found(); i++)
	      {
		if (strcmp(mactarget, metis_mac_address(i)) == 0)	// Exact match found
		{
		  metis_entry = i;					// Select entry in metis_table
	          found = true;
		  break;
		}
	      }
	}

	metis_receive_stream_control(RxStream_Off, metis_entry);	// turn off Hermes -> PC streams

	UpdateHermes();					// send specific control registers
							// and initialize 1st Tx buffer
							// before allowing scheduler to Start()
};

HermesProxy::~HermesProxy()
{
	fprintf(stderr, "\nLostRxBufCount = %lu  TotalRxBufCount = %lu"
		"  LostTxBufCount = %lu  TotalTxBufCount = %lu"
		"  CorruptRxCount = %lu  LostEthernetRx = %lu\n",
	        LostRxBufCount, TotalRxBufCount, LostTxBufCount,
		TotalTxBufCount, CorruptRxCount, LostEthernetRx);

	metis_receive_stream_control(RxStream_Off, metis_entry);	// stop Hermes data stream
	
	metis_stop_receive_thread();	// stop receive_thread & close socket

	for(int i=0; i<NUMTXBUFS; i++)
		delete [] TxBuf[i];

	for(int i=0; i<NUMRXIQBUFS; i++)
		delete [] RxIQBuf[i];
}


void HermesProxy::Stop()	// stop ethernet I/O
{
	metis_receive_stream_control(RxStream_Off, metis_entry);	// stop Hermes Rx data stream
	TxStop = true;					// stop Tx data to Hermes
};

void HermesProxy::Start()	// start rx stream
{
	TxStop = false;					// allow Tx data to Hermes
	metis_receive_stream_control(RxStream_NB_On, metis_entry);	// stop Hermes Rx data stream
};

void HermesProxy::PrintRawBuf(RawBuf_t inbuf)	// for debugging
{
	fprintf(stderr,"Raw buffer from Hermes:  0x000:: ");
	for(int i=0; i<8; i++)
	  fprintf(stderr,"%02X:",inbuf[i]); 
	fprintf(stderr,"\n");

	inbuf += 8;

	fprintf(stderr, " | 0x008:: ");
	for(int i=0; i<8; i++)
	  fprintf(stderr,"%02X:",inbuf[i]); 
	fprintf(stderr, "\n  ");

	for (int j=0; j<5; j++) {
	  for(int i=0; i<14; i++)
	    fprintf(stderr,"%02X:",inbuf[i+j*14+8]); 
	  fprintf(stderr, "\n  ");
	}
	
	fprintf(stderr, "\n");

	fprintf(stderr, " | 0x208:: ");
	for(int i=0; i<8; i++)
	  fprintf(stderr,"%02X:",inbuf[i+512]); 
	fprintf(stderr,"\n  ");

	for (int j=0; j<5; j++) {
	  for(int i=0; i<14; i++)
	    fprintf(stderr,"%02X:",inbuf[i+j*14+520]);
	  fprintf(stderr, "\n  ");
	}

	fprintf(stderr, "\n");


};

// ********** Routines to receive data from Hermes/Metis and give to Gnuradio ****************

void HermesProxy::ReceiveRxIQ(unsigned char * inbuf)	// called by metis Rx thread.
{

	// look for lost receive packets based on skips in the HPSDR ethernet header
	// sequence number.

	unsigned int SequenceNum = (unsigned char)(inbuf[4]) << 24;
	SequenceNum += (unsigned char)(inbuf[5]) << 16;
	SequenceNum += (unsigned char)(inbuf[6]) << 8;
	SequenceNum += (unsigned char)(inbuf[7]);

	if(SequenceNum > CurrentEthSeqNum + 1)
	{
	    LostEthernetRx += (SequenceNum - CurrentEthSeqNum);
	    CurrentEthSeqNum = SequenceNum;
	}
	else
	{
	  if(SequenceNum == CurrentEthSeqNum + 1)
	    CurrentEthSeqNum++;
	}
	

	// Metis Rx thread gives us collection of samples including the Ethernet header
	// plus 2 x HPSDR USB frames.

	// TODO - Handle Mic audio from Hermes.


	// For 1 Rx, the frame comes in with I2 I1 I0 Q2 Q1 Q0 M1 M0 repeating
	// starting at location 8 through 511. At total of (512-8)/8 = 63 complex pairs.
	// I2 I1 I0 is 24-bit 2's complement format.
	// There are two of the USB HPSDR frames in the received ethernet buffer.
	// A buffer of 126 complex pairs is about
	//	0.6 milliseconds at 192,000 sample rate
	//	2.4 milliseconds at 48,000 sample rate


	// We get 126 complex samples per input buffer, but Gnuradio seems happier
	// with 128 complex samples per output buffer; QtGUI is smoother, and the
	// work pipeline is faster.
	//
 	//    RxWriteCounter - the current Rx buffer we are writing to
	//    RxWriteFill    - #floats we have written to the current Rx buffer (0..255)
	//    RxReadCounter  - the Rx buffer that gnuradio can read
	//


	inbuf += 8;			// skip past Ethernet header

	IQBuf_t outbuf;			// RxWrite output buffer selector
	
	outbuf = RxIQBuf[RxWriteCounter];	// initialize buffer pointer

	TotalRxBufCount++;

	ScheduleTxFrame(TotalRxBufCount); // Schedule a Tx ethernet frame to Hermes if ready.

	// Need to check for both 1st and 2nd USB frames for the status registers.
	// Some status come in only in the first, and some only in the second.

	// check for proper frame sync

	for (int USBFrameOffset = 0; USBFrameOffset<=512; USBFrameOffset += 512)
	{

		unsigned char s0 = inbuf[0+USBFrameOffset];	// sync register 0
		unsigned char s1 = inbuf[1+USBFrameOffset];	// sync register 0
		unsigned char s2 = inbuf[2+USBFrameOffset];	// sync register 0
		unsigned char c0 = inbuf[3+USBFrameOffset];	// control register 0
		unsigned char c1 = inbuf[4+USBFrameOffset];	// control register 1
		unsigned char c2 = inbuf[5+USBFrameOffset];	// control register 2
		unsigned char c3 = inbuf[6+USBFrameOffset];	// control register 3
		unsigned char c4 = inbuf[7+USBFrameOffset];	// control register 4

		if(s0 == 0x7f && s1 == 0x7f && s2 == 0x7f)
		{
			if((c0 & 0xf8) == 0x00) // Overflow and Version
			{
//			  fprintf(stderr, "Reg:0x00   c0:0x%x c1:0x%x c2:0x%x c3:0x%u c4:0x%x\n", c0, c1, c2, c3, c4);

			  if(c1 & 0x01)
			    ADCoverload = true;
			  else
			    ADCoverload = false;

			  HermesVersion = c4;
			}

			if((c0 & 0xf8) == 0x08)  //AIN5 and AIN1
			{
//			  fprintf(stderr, "Reg:0x08   c0:0x%x c1:0x%x c2:0x%x c3:0x%u c4:0x%x\n", c0, c1, c2, c3, c4);
			  AIN5 = (unsigned int)c1 * 256 + (unsigned int)c2;
			  AIN1 = (unsigned int)c3 * 256 + (unsigned int)c4;
			}

			if((c0 & 0xf8) == 0x10)  //AIN2 and AIN3
			{
//			  fprintf(stderr, "Reg:0x10   c0:0x%x c1:0x%x c2:0x%x c3:0x%u c4:0x%x\n", c0, c1, c2, c3, c4);
			  AIN2 = (unsigned int)c1 * 256 + (unsigned int)c2;
			  AIN3 = (unsigned int)c3 * 256 + (unsigned int)c4;

			}

			if((c0 & 0xf8) == 0x18)  //AIN4 and AIN6
			{
//			  fprintf(stderr, "Reg:0x18   c0:0x%x c1:0x%x c2:0x%x c3:0x%u c4:0x%x\n", c0, c1, c2, c3, c4);
			  AIN4 = (unsigned int)c1 * 256 + (unsigned int)c2;
			  AIN6 = (unsigned int)c3 * 256 + (unsigned int)c4;
			}

			if (Verbose)
			{
			  SlowCount++;
			  if ((SlowCount & 0x1ff) == 0x1ff)
			  {
				float FwdPwr = (float)AIN1 * (float)AIN1 / 145000.0;
				float RevPwr = (float)AIN2 * (float)AIN2 / 145000.0;

				// calculate SWR
				double SWR =  0.0;
				try
				{
					SWR = (1+sqrt(RevPwr/FwdPwr))/(1-sqrt(RevPwr/FwdPwr));
					if(false == std::isnormal(SWR))
					{
						throw 0;
					}
				}
				catch(int& e)
				{
					// there was an anomaly in the SWR calculation, make it obvious ...
					SWR =  99.9;
				}

				fprintf(stderr, "AlexFwdPwr = %4.0f  AlexRevPwr = %4.0f   ", FwdPwr, RevPwr);
				// report SWR if forward power is non-zero
				if(static_cast<int>(FwdPwr) != 0)
				{
					fprintf(stderr, "SWR = %.2f:1 ", SWR);
				}
				fprintf(stderr, "ADCOver: %u  HermesVersion: %d (dec)  %X (hex)\n", ADCoverload, HermesVersion, HermesVersion);
				//fprintf(stderr, "AIN1:%u  AIN2:%u  AIN3:%u  AIN4:%u  AIN5:%u  AIN6:%u\n", AIN1, AIN2, AIN3, AIN4, AIN5, AIN6);  
				}
			}
		} //endif sync is valid
		
		else
		{
			CorruptRxCount++;
			//fprintf(stderr, "HermesProxy: EP6 received from Hermes failed sync header check.\n");
			//PrintRawBuf(inbuf-8);	// include Ethernet header
			return;
		}

	}	// end for two USB frames


	// Use write and read counters to select from the Rx buffers,
	// these are circular.

	if ((outbuf = GetNextRxBuf(outbuf)) == NULL)
	    return;			// all buffers full. Throw away data

	// Convert 24-bit 2's complement integer samples to float with
	// maximum value of +1.0 and minimum of -1.0
	// skip sync/register headers (i=0 and i=64)


	if (NumReceivers == 1)		// one receiver
	{					// 8 byte header + 8 bytes per row * 63 rows = 512 byte USB
	  for (int i=1; i<128; i++)	// both USB frames, skip header on first frame
	  {
	    if (i==64)			// skip header for 2nd frame
	      continue;

	    Unpack1RxIQ(&inbuf[i*8], outbuf);  // convert 2's comp to float and place in outbuf

	    if ((outbuf = GetNextRxBuf(outbuf)) == NULL)  // if needed, get next buffer
	        return;			// all buffers full. Throw away data      
	  } 
	}
	else				// two receivers
	{				// 8 byte header + 14 bytes per row * 36 rows = 512 byte USB
	//PrintRawBuf(inbuf-8);
	  for (int i=0; i<36; i++)	// first USB frame
	  {
	    Unpack2RxIQ(&inbuf[(i*14) + 8], outbuf);  // convert 2's comp to float and place in outbuf

	    if ((outbuf = GetNextRxBuf(outbuf)) == NULL)  // if needed, get next buffer
	        return;			// all buffers full. Throw away data
	  } 
	  for (int i=0; i<36; i++)	// second USB frame
	  {
	    Unpack2RxIQ(&inbuf[(i*14) + 520], outbuf);  // convert 2's comp to float and place in outbuf

	    if ((outbuf = GetNextRxBuf(outbuf)) == NULL)  // if needed, get next buffer
	      return;			// all buffers full. Throw away data
	  }
	}

	return;			// normal return;

};


IQBuf_t HermesProxy::GetNextRxBuf(IQBuf_t current_outbuf) // get new Rx buffer if we've filled current one
{

	//pthread_mutex_lock(&mutexRPG);

	//if(RxWriteFill > RXBUFSIZE)
	//  fprintf(stderr, "ERROR: RxWriteFill: %d  Overflow\n",RxWriteFill);

	if(RxWriteFill & RXBUFSIZE)  // need a new buffer?
	{
	  if (((RxWriteCounter+1) & (NUMRXIQBUFS - 1)) == RxReadCounter)
	  {
		LostRxBufCount++;	// No Rx Buffers available. Throw away the data
	  	//pthread_mutex_unlock(&mutexRPG);
		return NULL;
	  }
	  ++RxWriteCounter &= (NUMRXIQBUFS - 1); // get next writeable buffer
	  RxWriteFill = 0;

	  //pthread_mutex_unlock(&mutexRPG);
	  return RxIQBuf[RxWriteCounter];
	}
	else				// don't need a new buffer
	{
	  //pthread_mutex_unlock(&mutexRPG);
	  return current_outbuf;
	}
};

void HermesProxy::Unpack1RxIQ(const unsigned char* inptr, const IQBuf_t outbuf)
{
	// Unpack 8 bytes in the HPSDR USB frame to I and Q for 1 receiver.
	// 24 bit 2's complement --> float (-1.0 ... +1.0)
	// Ignore the 16-bit Mic sample from Hermes/Metis

	if ((PTTOnMutesRx) & (PTTMode == PTTOn))
	{
	  outbuf[RxWriteFill++] = 0.0;
	  outbuf[RxWriteFill++] = 0.0;
	  return;
	}	

	int I, Q;

	I = (int)(((signed char)*inptr)<<16);		// Rx0 I
	inptr++;
	I += ((int)((unsigned char)*inptr)<<8);
	inptr++;
	I += (int)((unsigned char)*inptr);
	inptr++;
	if(I<0) I = -(~I + 1);

	Q = (int)(((signed char)*inptr)<<16);		// Rx0 Q
	inptr++;
	Q += ((int)((unsigned char)*inptr)<<8);
	inptr++;
	Q += (int)((unsigned char)*inptr);
	inptr++;
	if(Q<0) Q = -(~Q + 1);

	outbuf[RxWriteFill++] = (float)I/8388607.0;
	outbuf[RxWriteFill++] = (float)Q/8388607.0;

	// inptr+= 2;					// skip Mic samples
};

void HermesProxy::Unpack2RxIQ(const unsigned char* inptr, const IQBuf_t outbuf)
{
	// Unpack 14 bytes in the HPSDR USB frame to I and Q for 2 receivers.
	// 24 bit 2's complement --> float (-1.0 ... +1.0)
	// Ignore the 16-bit Mic sample from Hermes/Metis

	if ((PTTOnMutesRx) & (PTTMode == PTTOn))
	{
	  outbuf[RxWriteFill++] = 0.0;
	  outbuf[RxWriteFill++] = 0.0;
	  outbuf[RxWriteFill++] = 0.0;
	  outbuf[RxWriteFill++] = 0.0;
	  return;
	}	

	int I, Q, I1, Q1;
	float If, Qf, I1f, Q1f;

	I = (int)(((signed char)*inptr)<<16);		// Rx0 I
	inptr++;
	I += ((int)((unsigned char)*inptr)<<8);
	inptr++;
	I += (int)((unsigned char)*inptr);
	inptr++;
	if(I<0) I = -(~I + 1);
	If = (float)I/8388607.0;

	Q = (int)(((signed char)*inptr)<<16);		// Rx0 Q
	inptr++;
	Q += ((int)((unsigned char)*inptr)<<8);
	inptr++;
	Q += (int)((unsigned char)*inptr);
	inptr++;
	if(Q<0) Q = -(~Q + 1);
	Qf = (float)Q/8388607.0;


	I1 = (int)(((signed char)*inptr)<<16);		// Rx1 I
	inptr++;
	I1 += ((int)((unsigned char)*inptr)<<8);
	inptr++;
	I1 += (int)((unsigned char)*inptr);
	inptr++;
	if(I1<0) I1 = -(~I1 + 1);
	I1f = (float)I1/8388607.0;

	Q1 = (int)(((signed char)*inptr)<<16);		// Rx1 Q
	inptr++;
	Q1 += ((int)((unsigned char)*inptr)<<8);
	inptr++;
	Q1 += (int)((unsigned char)*inptr);
	inptr++;
	if(Q1<0) Q1 = -(~Q1 + 1);
	Q1f = (float)Q1/8388607.0;

	outbuf[RxWriteFill++] = If;
	outbuf[RxWriteFill++] = Qf;
	outbuf[RxWriteFill++] = I1f;
	outbuf[RxWriteFill++] = Q1f;

	// inptr+= 2;					// skip Mic samples

	return;
};

IQBuf_t HermesProxy::GetRxIQ()		// called by HermesNB to pickup any RxIQ
{

	//int status = pthread_mutex_trylock(&mutexRPG);	// Don't block gnuradio scheduler
	//  if(status != 0)
	//    return NULL;		// return 'no buffers' if can't acquire the mutex

	if(RxReadCounter == RxWriteCounter)
	{
	  //pthread_mutex_unlock(&mutexRPG);
	  return NULL;				// empty - no buffers to return

	}

	IQBuf_t ReturnBuffer = RxIQBuf[RxReadCounter];	// get the next receiver buffer
	++RxReadCounter &= (NUMRXIQBUFS - 1);		// increment read counter modulo

	//pthread_mutex_unlock(&mutexRPG);

	return ReturnBuffer;
};


// ************  Routines to send data from gnuradio to the transmitter ***************


// The Hermes hardware does not have any method to indicate when it wants a frame,
// nor any back pressure mechanism. We derive the Tx timing by counting the Rx frames
// Hermes is sending to us. This depends on the Rx Sample rate and the number of
// receivers because the Tx sample rate is fixed at 48000.
//
// Sample rate		# Receivers		# Receive frames for one Tx frame
// -----------		-----------		---------------------------------
//    48000			1				1
//    48000			2				1.75
//    96000			1				2
//    96000			2				3.5
//   192000			1				4
//   192000			2				7
//   384000			1				8
//   384000			2				14
//
//
// If no data to transmit, periodically send a frame so that basic control registers
// get updated.   [Hooks left commented for future use].


void HermesProxy::ScheduleTxFrame(unsigned long RxBufCount) // Transmit one ethernet frame to Hermes if ready.
{

	if(NumReceivers == 1)		// one receiver
	{
	  if(RxSampleRate == 48000)	// one Tx frame for each Rx frame
	  {
		SendTxIQ();
		return;
	  }

	  if(RxSampleRate == 96000)	// one Tx frame for each two Rx frames
	    if((RxBufCount & 0x1) == 0)
	    {
		SendTxIQ();
		return;
	    }
	
	  if(RxSampleRate == 192000)	// one Tx frame for each four Tx frames
	    if((RxBufCount & 0x3) == 0)
	    {
		SendTxIQ();
		return;
	    }

	  if(RxSampleRate == 384000)	// one Tx frame for each eight Tx frames
	    if((RxBufCount & 0x7) == 0)
	    {
		SendTxIQ();
		return;
	    }
	}
	else				// two receivers
	{
	  if(RxSampleRate == 48000)			// one Tx frame for each 1.75 Rx frame
	    if(((RxBufCount % 0x7) & 0x01) == 0)    	// (four Tx frames for each 7 Rx frames)
	    {
	        SendTxIQ();				// 0, 2, 4, 6   (not 1, 3, 5)
		return;
	    }

	  if(RxSampleRate == 96000)			// one Tx frame for each 3.5 Rx frames
	    if(((RxBufCount % 0x7) & 0x03) == 0) 	// (two Tx frames for each 7 Rx frames)
	    {
		SendTxIQ();				// 0, 4    (not 1, 2, 3, 5, 6)
		return;
	    }
	
	  if(RxSampleRate == 192000)			// one Tx frame for each seven Tx frames
	    if((RxBufCount % 0x7) == 0)
	    {
		SendTxIQ();
		return;
	    }
	
	  if(RxSampleRate == 384000)			// one Tx frame for each fourteen Tx frames
	    if((RxBufCount % 14) == 0)
	    {
		SendTxIQ();
		return;
	    }
	}
	return;
};

void HermesProxy::UpdateHermes()	// send a set of control registers to hardware with naught Tx data
{

	// Repurposed to send the initial registers to Hermes before starting the stream.
	// Ought to rename this as InitializeHermes or something similar.

	// DEBUG
	//fprintf(stderr, "UpdateHermes called\n");

	unsigned char buffer[512];	// dummy up a USB HPSDR buffer;
	for(int i=0; i<512; i++)
		buffer[i] = 0;

	int length = 512;		// metis_write ignores this value
	unsigned char ep = 0x02;	// all Hermes data is sent to end point 2

	// metis_write needs to be called twice to make one ethernet write to the hardware
	// Set these registers before starting the receive stream

	BuildControlRegs(0, buffer);
	metis_write(ep, buffer, length);
	BuildControlRegs(2, buffer);
	metis_write(ep, buffer, length);

	BuildControlRegs(0, buffer);
	metis_write(ep, buffer, length);
	BuildControlRegs(4, buffer);
	metis_write(ep, buffer, length);

	BuildControlRegs(0, buffer);
	metis_write(ep, buffer, length);
	BuildControlRegs(6, buffer);
	metis_write(ep, buffer, length);

	// Initialize the first TxBuffer (currently empty) with a valid control frame (on startup only)
	
	BuildControlRegs(0, buffer);
	RawBuf_t initial = TxBuf[0];
	for(int i=0; i<512; i++)
		initial[i] = buffer[i];

	return;
}


void HermesProxy::BuildControlRegs(unsigned RegNum, RawBuf_t outbuf)
{
	// create the sync + control register values to send to Hermes
	// base on RegNum and the various parameter values.
	// RegNum must be even.

	unsigned char Speed = 0;	// Rx sample rate
	unsigned char RxCtrl = 0;	// Rx controls
	unsigned char Ctrl4 = 0;	// Rx register C4 control

	outbuf[0] = outbuf[1] = outbuf[2] = 0x7f;	// HPSDR USB sync

	outbuf[3] = RegNum;		// C0 Control Register (Bank Sel + PTT)
	if (PTTMode == PTTOn)
	  outbuf[3] |= 0x01;				// set MOX bit

	switch(RegNum)
	{
	  case 0:
	    Speed = ClockSource;	// Set clock Source from user input
	    if(RxSampleRate == 384000)
		Speed |= 0x03;
	    if(RxSampleRate == 192000)
		Speed |= 0x02;
	    if(RxSampleRate == 96000)
		Speed |= 0x01;
	    if(RxSampleRate == 48000)
		Speed |= 0x00;

	    RxCtrl = 0x00;
	    if(RxPreamp)
		RxCtrl |= 0x04;
	    if(ADCdither)
		RxCtrl |= 0x08;
	    if(ADCrandom)
		RxCtrl |= 0x10;

	    if(NumReceivers == 2)
		Ctrl4 |= 0x08;
	    if(Duplex)
		Ctrl4 |= 0x04;

	    outbuf[4] = Speed;				// C1
	    outbuf[5] = 0x00;				// C2
	    outbuf[6] = RxCtrl | AlexRxAnt;		// C3
	    outbuf[7] = Ctrl4 | AlexTxAnt;		// C4 - #Rx, Duplex
          break;

	  case 2:					// Tx NCO freq (and Rx1 NCO for special case)
	    outbuf[4] = ((unsigned char)(TransmitFrequency >> 24)) & 0xff;	// c1 RxFreq MSB
	    outbuf[5] = ((unsigned char)(TransmitFrequency >> 16)) & 0xff;	// c2
	    outbuf[6] = ((unsigned char)(TransmitFrequency >> 8)) & 0xff;	// c3
	    outbuf[7] = ((unsigned char)(TransmitFrequency)) & 0xff;		// c4 RxFreq LSB
          break;

	  case 4:					// Rx1 NCO freq
	    outbuf[4] = ((unsigned char)(Receive0Frequency >> 24)) & 0xff;	// c1 RxFreq MSB
	    outbuf[5] = ((unsigned char)(Receive0Frequency >> 16)) & 0xff;	// c2
	    outbuf[6] = ((unsigned char)(Receive0Frequency >> 8)) & 0xff;	// c3
	    outbuf[7] = ((unsigned char)(Receive0Frequency)) & 0xff;	// c4 RxFreq LSB
	  break;

	  case 6:					// Rx2 NCO freq
	    outbuf[4] = ((unsigned char)(Receive1Frequency >> 24)) & 0xff; // c1 RxFreq MSB
	    outbuf[5] = ((unsigned char)(Receive1Frequency >> 16)) & 0xff; // c2
	    outbuf[6] = ((unsigned char)(Receive1Frequency >> 8)) & 0xff;	 // c3
	    outbuf[7] = ((unsigned char)(Receive1Frequency)) & 0xff;	 // c4 RxFreq LSB
	  break;

	  case 8:					// Rx3 NCO freq
	  case 10:					// Rx4 NCO freq
	  case 12:					// Rx5 NCO freq
	  case 14:					// Rx6 NCO freq
	  case 16:					// Rx7 NCO freq
	    outbuf[4] = 0;				// c1 RxFreq MSB
	    outbuf[5] = 0;				// c2
	    outbuf[6] = 0;				// c3
	    outbuf[7] = 0;				// c4 RxFreq LSB
	  break;

	  case 18:					// drive level & filt select (if Alex)
	    if (PTTOffMutesTx & (PTTMode == PTTOff))
		outbuf[4] = 0;				// (almost) kill Tx when PTTOff and PTTControlsTx
	    else
		outbuf[4] = TxDrive;			// c1


	    unsigned char RxHPF, TxLPF;

	    RxHPF = AlexRxHPF;
	    if (AlexRxHPF == 0)				// if Rx autotrack
	    {
		if (Receive0Frequency < 1500000)
		  RxHPF = 0x20;				// bypass
		else if (Receive0Frequency < 6500000)
	          RxHPF = 0x10;				// 1.5 MHz HPF
		else if (Receive0Frequency < 9500000)
		  RxHPF = 0x08;				// 6.5 MHz HPF
		else if (Receive0Frequency < 13000000)
		  RxHPF = 0x04;				// 9.5 mHz HPF
		else if (Receive0Frequency < 20000000)
		  RxHPF = 0x01;				// 13 Mhz HPF
		else if (Receive0Frequency < 50000000)
		  RxHPF = 0x02;				// 20 MHz HPF
		else RxHPF = 0x40;			// 6M BPF + LNA
	    }

	    TxLPF = AlexTxLPF;
	    if (AlexTxLPF == 0)				// if Tx autotrack
	    {
		if (TransmitFrequency > 30000000)
		  TxLPF = 0x10;				// 6m LPF
		else if (TransmitFrequency > 19000000)
		  TxLPF = 0x20;				// 10/12m LPF
		else if (TransmitFrequency > 14900000)
		  TxLPF = 0x40;				// 15/17m LPF
		else if (TransmitFrequency > 9900000)
		  TxLPF = 0x01;				// 30/20m LPF
		else if (TransmitFrequency > 4900000)
		  TxLPF = 0x02;				// 60/40m LPF
		else if (TransmitFrequency > 3400000)
		  TxLPF = 0x04;				// 80m LPF
		else TxLPF = 0x08;			// 160m LPF
	    }

	    outbuf[5] = 0x40;				// c2 - Alex Manual filter control enabled
	    outbuf[6] = RxHPF & 0x7f;			// c3 - Alex HPF filter selection
	    outbuf[7] = TxLPF & 0x7f;			// c4 - Alex LPF filter selection
	  break;

	  case 20:					// Hermes input attenuator setting
	    outbuf[4] = 0;				//
	    outbuf[5] = 0x17;				// Not implemented yet, should not be called by
	    outbuf[6] = 0;				// TxControlCycler yet.
	    outbuf[7] = RxAtten;			// 0..31 db attenuator setting (same function as preamp)
	  break;
	
	  case 22:
	    outbuf[4] = 0;				// Register not documented, but zeroed by
	    outbuf[5] = 0;				// PowerSDR...
	    outbuf[6] = 0;				//
	    outbuf[7] = 0;				//
	  break;					

	  default:
	    fprintf(stderr, "Invalid Hermes/Metis register selection: %d\n", RegNum);
	    break;
	};

};


// hermesNB calls this routine to give IQ data from the block input connector to the proxy.
// Packs transformed data into one HPSDR USB buffer with control registers.
// HermesNB gives us 63 complex samples from in0, we fill one USB buffer with them.
// Audio output could come from in1 but that forms a flowgraph loop in most useful cases
// which is disallowed by GNU Radio, so that code is commented out.

int HermesProxy::PutTxIQ(const gr_complex * in0, /*const gr_complex * in1,*/ int nsamples) // called by HermesNB to give us IQ data to send
{

        RawBuf_t outbuf;
	int A, B, I, Q;

	outbuf = GetNextTxBuf();	// get a Txbuffer

	if (outbuf == NULL)		// Could not get a Tx buffer
	  return 0;		 	// Tell hermeNB we didn't consume any input

	// format a HPSDR USB frame to send to Hermes.

	TxControlCycler += 2;		// advance to next register bank, modulo
	if (TxControlCycler > 0x14)	// 11 register banks (0..10). Note: Bank 10
	  TxControlCycler = 0;		//    (Hermes attenuator) requires firmware V2.0

	BuildControlRegs(TxControlCycler, outbuf);	// First 8 bytes are the control registers.


	// Next 63 * 8 bytes are the IQ data and the Audio data.
	// TODO - the L/R audio data to Hermes is not implemented yet.


	for (int i=0; i<nsamples; i++)			// put 63 IQ samples into frame
        {

	// Note: cannot implement audio output because the flowgraph would form a flow loop
	// for any Hermes received data which is not allowed in GNU Radio.

/*	  A = (int)(in1[i].real() * 32767.0);	// scale to 16 bits
	  B = (int)(in1[i].imag() * 32767.0);	// scale to 16 bits
          I = (unsigned int)A;
	  Q = (unsigned int)B;

 	  // convert float to 2's complement 16-bit

	  outbuf[i*8 + 8] = (unsigned char)((I & 0xff00) >> 8);  // L1 MSB audio channel out
	  outbuf[i*8 + 9] = (unsigned char)(I & 0xff);		 // L0 LSB
	  outbuf[i*8 + 10] = (unsigned char)((Q & 0xff00) >> 8); // R1 MSB audio channel out
	  outbuf[i*8 + 11] = (unsigned char)(Q & 0xff);		 // R0 LSB
*/

	// Zero out the audio Left and Right channel outputs.

	  outbuf[i*8 + 8] = 0;		// L1 MSB audio channel out
	  outbuf[i*8 + 9] = 0;		// L0 LSB
	  outbuf[i*8 + 10] = 0;		// R1 MSB audio channel out
	  outbuf[i*8 + 11] = 0;		// R0 LSB   

 	  // convert float to 2's complement 16-bit

	  A = (int)(in0[i].real() * 32767.0);	// scale to 16 bits
	  B = (int)(in0[i].imag() * 32767.0);	// scale to 16 bits


//        I = (unsigned int)A;
//	  Q = (unsigned int)B;
// 03-13-2014  Note: Hermes FPGA reverses transmit I & Q (thus contrary to documentation V1.43)
// Put them back into the correct places.
          Q = (unsigned int)A;
	  I = (unsigned int)B;



	  if(PTTOffMutesTx & (PTTMode == PTTOff))	// Kill Tx if in Rx and PTTControls the Tx
	  {
	    I = 0;
	    Q = 0;
	  };

	  outbuf[i*8 + 12] = (unsigned char)((I & 0xff00) >> 8); // I1 MSB
	  outbuf[i*8 + 13] = (unsigned char)(I & 0xff);		 // I0 LSB
	  outbuf[i*8 + 14] = (unsigned char)((Q & 0xff00) >> 8); // Q1 MSB
	  outbuf[i*8 + 15] = (unsigned char)(Q & 0xff);		 // Q0 LSB

        };


	if(PTTMode == PTTVox)		// if we are in Vox mode, check frame IQ contents
	{
          bool activity = false;

	  for (int i=0; i<nsamples; i++)	 // if any IQ sample is nonzero (VOX) then key Tx
	    if ((outbuf[i*8 + 12] != 0) ||  
	        (outbuf[i*8 + 13] != 0) ||
	        (outbuf[i*8 + 14] != 0) ||
	        (outbuf[i*8 + 15] != 0) )
	    {
		activity = true;
		break;
	    };

	    if(activity)
		outbuf[3] |= 1;		// enable MOX PTT	    
	};

//	fprintf(stderr, "PutTxIQ: Consumed %d samples, TxControlCycler = %d\n",
//		nsamples, TxControlCycler);


	return nsamples;
};


RawBuf_t HermesProxy::GetNextTxBuf()		// get a TXBuf if available
{

	  //int status = pthread_mutex_trylock(&mutexGPT); // Don't block gnuradio scheduler
	  //if(status != 0)
	 //   return NULL;	// No buffers if can't acquire mutex

	  if (((TxWriteCounter+1) & (NUMTXBUFS - 1)) == TxReadCounter)
	  {
	    //pthread_mutex_unlock(&mutexGPT);
	    return NULL;
	  }
	 
	  ++TxWriteCounter &= (NUMTXBUFS - 1); // get next writeable buffer

	  //pthread_mutex_unlock(&mutexGPT);
	  return TxBuf[TxWriteCounter];
};


// SendTxIQ() is called on a periodic basis to send Tx Ethernet frames to the 
// Hermes/Metis hardware.
void HermesProxy::SendTxIQ()
{

	if(TxStop)				// Kill Tx frames if stopped
		return;

	unsigned char ep = 0x2;			// Tx data goes to end point 2

//	fprintf(stderr, "SendTxIQ: TxReadCounter = %d   TxWriteCounter = %d  TxFrameIdleCount = %d\n",
//		TxReadCounter, TxWriteCounter, TxFrameIdleCount); 

	// Time to send one Tx Eth frame (2 x USB frames).
	// If there are at least two buffers in the queue, send then free them.

	//pthread_mutex_lock(&mutexGPT);

	bool bufempty = (TxReadCounter == TxWriteCounter);
	bool bufone = ((TxReadCounter+1 & (NUMTXBUFS - 1)) == TxWriteCounter);

	//pthread_mutex_unlock(&mutexGPT);

	TotalTxBufCount++;

	if ( bufempty | bufone )    // zero or one buffer ready	
	{
	  LostTxBufCount++;

	//fprintf(stderr, "SendTxIQ01: TxReadCounter = %d   TxWriteCounter = %d  "
		//"TxFrameIdleCount = %d  bufempty = %d   bufone = %d\n",
		//TxReadCounter, TxWriteCounter, TxFrameIdleCount, bufempty, bufone); 

	 // if(TxFrameIdleCount++ > 400)
	 // {
	    //UpdateHermes();             // Force basic register update if 
	 //   TxFrameIdleCount = 0;	//   we've not sent for about 1 second
	 // }
	}
	else	// two or more buffers ready
	{

	//fprintf(stderr, "SendTxIQ02: TxReadCounter = %d   TxWriteCounter = %d  TxFrameIdleCount = %d\n",
		//TxReadCounter, TxWriteCounter, TxFrameIdleCount); 

	  metis_write(ep, TxBuf[TxReadCounter], 512);	// write one USB frame to metis

	 // pthread_mutex_lock(&mutexGPT);
	  ++TxReadCounter &= (NUMTXBUFS - 1);		// and free it
	  //pthread_mutex_unlock(&mutexGPT);

	  metis_write(ep, TxBuf[TxReadCounter], 512);	// write next USB frame to metis

	  //pthread_mutex_lock(&mutexGPT);
	  ++TxReadCounter &= (NUMTXBUFS - 1);		// and free it
	  //pthread_mutex_unlock(&mutexGPT);

	 // TxFrameIdleCount = 0;				// have just sent a frame
	};

	return;
};


// TODO not yet implemented
void HermesProxy::ReceiveMicLR() {};	// receive an LR audio bufer from Hermes hardware


