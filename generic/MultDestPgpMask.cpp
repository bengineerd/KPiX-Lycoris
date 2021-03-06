//-----------------------------------------------------------------------------
// File          : MultDestPgpMask.cpp
// Author        : Ryan Herbst  <rherbst@slac.stanford.edu>
// Created       : 06/18/2014
// Project       : General Purpose
//-----------------------------------------------------------------------------
// Description :
// PGP Destination container for MultLink class.
//
// LinkConfig Field usage:
// bits 7:0 = Index (ignored)
// bits 11:8 =  PGP VC for register transactions
// bits 15:12 = PGP Lane for register transactions
// bits 19:16 = PGP VC for commands
// bits 23:20 = PGP Lane for commands
// bits 27:24 = PGP VC for data
// bits 31:28 = PGP Lane for data
//-----------------------------------------------------------------------------
// This file is part of 'SLAC Generic DAQ Software'.
// It is subject to the license terms in the LICENSE.txt file found in the 
// top-level directory of this distribution and at: 
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html. 
// No part of 'SLAC Generic DAQ Software', including this file, 
// may be copied, modified, propagated, or distributed except according to 
// the terms contained in the LICENSE.txt file.
// Proprietary and confidential to SLAC.
//-----------------------------------------------------------------------------
// Modification history :
// 06/18/2014: created
//-----------------------------------------------------------------------------
#include <MultDestPgpMask.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
#include <string.h>
#include <iomanip>
#include <PgpCardModMask.h>
#include <PgpCardWrapMask.h>
#include <sstream>
#include <Register.h>
#include <Command.h>
#include <stdint.h>
using namespace std;

//! Constructor
MultDestPgpMask::MultDestPgpMask (string path, uint mask) : MultDest(512) { 
   path_ = path;
   mask_ = mask;
}

//! Deconstructor
MultDestPgpMask::~MultDestPgpMask() { 
   this->close();
}

//! Open link
void MultDestPgpMask::open ( uint32_t idx, uint32_t maxRxTx ) {
   stringstream tmp;

   this->close();

   fd_ = ::open(path_.c_str(),O_RDWR | O_NONBLOCK);

   if ( fd_ < 0 ) {
      tmp.str("");
      tmp << "MultDestPgpMask::open -> Could Not Open PGP path " << path_;
      throw tmp.str();
   }

   if ( pgpcard_setMask(fd_,mask_) != 0 ) {
      tmp.str("");
      tmp << "MultiDestPgp::open -> Error setting mask" << endl;
      throw tmp.str();
   }

   if ( debug_ ) 
      cout << "MultDestPgpMask::open -> Opened pgp device " << path_
           << ", with mask=" << mask_
           << ", Fd=" << dec << fd_ << endl;

   MultDest::open(idx,maxRxTx);
}

//! Transmit data.
int32_t MultDestPgpMask::transmit ( MultType type, void *ptr, uint32_t size, uint32_t context, uint32_t config ) {
   uint32_t   lane;
   uint32_t   vc;
   Register * reg;
   Command  * cmd;
   bool       isWrite;
   uint32_t   txSize;
   uint32_t * txData;
   int32_t    ret;

   isWrite = false;

   // Types
   switch ( type ) {
      case MultTypeRegisterWrite :
         isWrite = true;
      case MultTypeRegisterRead  :
         txData  = (uint32_t *)txData_;
         reg     = (Register*)ptr;
         lane    = (config >> 12) & 0xf;
         vc      = (config >>  8) & 0xf;

         // Setup buffer
         txData[0]  = context;
         txData[1]  = (isWrite)?0x40000000:0x00000000;
         txData[1] |= (reg->address() >> 2) & 0x3FFFFFFF; // Drop lower 2 address bits

         // Write has data
         if ( isWrite ) {
            memcpy(&(txData[2]),reg->data(),(reg->size()*4));
            txData[reg->size()+2] = 0;
            txSize = reg->size()+3;
         }

         // Read is always small
         else {
            txData[2]  = (reg->size()-1);
            txData[3]  = 0;
            txSize = 4;
         }
         break;

      case MultTypeCommand :
         txData = (uint32_t *)txData_;
         cmd    = (Command*)ptr;
         lane   = (config >> 20) & 0xf;
         vc     = (config >> 16) & 0xf;

         // Setup buffer
         txData[0]  = 0;
         txData[1]  = cmd->opCode() & 0xFF;
         txData[2]  = 0;
         txData[3]  = 0;
         txSize     = 4;
         break;

      case MultTypeData :
         lane   = (config >> 28) & 0xf;
         vc     = (config >> 24) & 0xf;
         txData = (uint32_t *)ptr;
         txSize = size/4;
         break;
   }

   ret = pgpcard_send(fd_, txData, txSize, lane, vc);
   while(ret < 0 ){
      ret = pgpcard_send(fd_, txData, txSize, lane, vc);
   }

   if ( ret > 0 ) ret = ret * 4;
   return(ret);
}

// Receive data
int32_t MultDestPgpMask::receive ( MultType *type, void **ptr, uint32_t *context ) {
   int32_t  ret;
   uint32_t lane;
   uint32_t vc;
   uint32_t eofe;
   uint32_t fifoErr;
   uint32_t lengthErr;
   uint32_t dataSource;
   uint32_t * rxData;
   
   rxData = (uint32_t *)rxData_;

   // attempt receive
   ret = pgpcard_recv(fd_, rxData, (dataSize_/4), &lane, &vc, &eofe, &fifoErr, &lengthErr);

   // No data
   if ( ret == 0 ) return(0);

   // Bad size or error
   if ( ret < 4 || eofe || fifoErr || lengthErr ) {
      if ( debug_ ) {
         cout << "MultDestPgpMask::receive -> "
              << "Error in data receive. Rx=" << dec << ret
              << ", Lane=" << dec << lane << ", Vc=" << dec << vc
              << ", EOFE=" << dec << eofe << ", FifoErr=" << dec << fifoErr
              << ", LengthErr=" << dec << lengthErr << endl;
      }
      return(-1);
   }

   dataSource  = (lane << 28) & 0xF0000000;
   dataSource += (vc   << 24) & 0x0F000000;

   // Is this a data receive?
   if ( isDataSource(dataSource) ) {
      *ptr = rxData;
      *context = 0;
      *type  = MultTypeData;
   }

   // Otherwise this is a register receive
   else {
      *ptr = rxRegister_;
      
      // Setup buffer
      *context = rxData[0];
      rxRegister_->setAddress(rxData[1] << 2);

      // Set type
      if ( rxData[1] & 0x40000000 ) *type = MultTypeRegisterWrite;
      else *type = MultTypeRegisterRead;

      // Double check size
      if ( ret-3 > (int32_t)rxRegister_->size() ) {
         if ( debug_ ) {
            cout << "MultDestPgpMask::receive -> "
                 << "Bad size in register receive. Address = 0x" 
                 << hex << setw(8) << setfill('0') << rxRegister_->address()
                 << ", RxSize=" << dec << (ret-3)
                 << ", Max Size=" << dec << rxRegister_->size() << endl;
         }
         return(-1);
      }

      // Copy data and status
      memcpy(rxRegister_->data(),&(rxData[2]),(ret-3)*4);
      rxRegister_->setStatus(rxData[ret-1]);
   }

   return(ret*4);
}

