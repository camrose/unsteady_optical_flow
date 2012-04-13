/*
 * Copyright (c) 2010, Regents of the University of California
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the University of California, Berkeley nor the names
 *   of its contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 *
 * Software module for AT86RF231 (SPI)
 *
 * by Stanley S. Baek
 *
 * v.beta
 *
 * Revisions:
 *  Stanley S. Baek      2010-06-05    Initial release
 *                      
 * Notes:
 *  SPI1 is used for AT86RF231
 *  SPI2 should be used if you run this module on
 *  MikroElektronika dev board because RB2 is used for LCD.
 *  
 * TODO: 
 *  1. Implement DMA for SPI.
 * 
 */

#include <stdlib.h>
#include <stdarg.h>     // variable number of arguments
#include "ports.h"      // for external interrupt
#include "utils.h"
#include "spi.h"
#include "radio.h"
#include "at86rf231.h"
#include "payload_queue.h"
#include "payload.h"
#include "generic_typedefs.h"

#include "lcd.h"
#include <stdio.h>


#if defined(__MIKRO)
    #define SPI_BUF         SPI2BUF
    #define SPI_CON1bits    SPI2CON1bits
    #define SPI_CON2        SPI2CON2
    #define SPI_STATbits    SPI2STATbits
    #define SPI_CS          _LATG9
    #define SLPTR           _LATF0      //_LATB15
#elif defined(__EXP16DEV)
    #define SPI_BUF         SPI2BUF
    #define SPI_CON1bits    SPI2CON1bits
    #define SPI_CON2        SPI2CON2
    #define SPI_STATbits    SPI2STATbits
    #define SPI_CS          _LATG9
    #define SLPTR           _LATB1
#elif defined(__BASESTATION)
    #define SPI_BUF         SPI2BUF
    #define SPI_CON1bits    SPI2CON1bits
    #define SPI_CON2        SPI2CON2
    #define SPI_STATbits    SPI2STATbits
    #define SPI_CS          _LATG9
    #define SLPTR           _LATE5
#else
    #define SPI_BUF         SPI1BUF
    #define SPI_CON1bits    SPI1CON1bits
    #define SPI_CON2        SPI1CON2
    #define SPI_STATbits    SPI1STATbits
    #define SPI_CS          _LATB2
    #define SLPTR           _LATB15
#endif



typedef struct {
    union {
        WordVal    val;   
        struct  {

            // type of packet. Possible types are
            // * PACKET_TYPE_DATA - Data type
            // * PACKET_TYPE_COMMAND -  Command type
            // * PACKET_TYPE_ACK -  Acknowledgement type
            // * PACKET_TYPE_RESERVE - Reserved type
            word    packetType      : 3;     

            // 1: secure the MAC payload, 0: send plain text
            // We are *not* currently implementing secured transmission
            word    secEn           : 1;   

            // 1: sending device has more data for recipient, 0: no more data
            word    frmPending      : 1;        
            
            // 1: acknowledgement required, 0: no acknowldgement
            word    ackReq          : 1;        
            
            // 1: PAN ID compression subfield
            word    panIDComp       : 1;     

            word    reserved        : 3;

            // 0: PAN ID and dest addr not present, 
            // 1: reserved, 
            // 2: address field contains 16 bit short address 
            // 3: address field contains 64 bit extended address - NOT IMPLEMENTED
            word    destAddrMode    : 2;    

            // 0: IEEE 802.15.4-2003 
            // 1: IEEE 802.15.4
            word    frmVersion      : 2;        

            // 0: PAN ID and src addr not present, 
            // 1: reserved, 
            // 2: address field contains 16 bit short address 
            // 3: address field contains 64 bit extended address             
            word    srcAddrMode     : 2;        
        } bits;
    } frame_ctrl;

    byte    seqNum;

    WordVal destPANID;  
    
    //For the moment we are not implementing extended 64-bit addresses
    WordVal destAddr;   
    
    WordVal srcPANID;
    
    //For the moment we are not implementing extended 64-bit addresses
    //byte * auxSecHdr  //Placeholder for auxiliary security header when we
                        //get around to implementing it.
    WordVal srcAddr;    

    Payload payload;
    
    byte    payloadLength;

} MacPacketStruct;

typedef MacPacketStruct* MacPacket;



#define TRX_CMD_RW          (0xC0) // Register Write
#define TRX_CMD_RR          (0x80) // Register Read 
#define TRX_CMD_FW          (0x60) // Frame Transmit Mode
#define TRX_CMD_FR          (0x20) // Frame Receive Mode
#define TRX_CMD_SW          (0x40) // SRAM Write.
#define TRX_CMD_SR          (0x00) // SRAM Read.

#define MAX_FRAME_LEN       (127)


// Based on 16-bit addressing for PAN and device and no 
// auxiliary security header
#define MAC_HEADER_LENGTH       9
#define CRC_LENGTH              2


// default value for MAC HEADER
#define DEFAULT_CHANNEL         0x16
#define DEFAULT_DEST_PAN_ID     0x2020
#define DEFAULT_SRC_PAN_ID      0x2020
#define DEFAULT_DEST_ADDR       0x2021
#define DEFAULT_SRC_ADDR        0x2022

// packet types
#define PACKET_TYPE_BEACON      0x00
#define PACKET_TYPE_DATA        0x01
#define PACKET_TYPE_ACK         0x02
#define PACKET_TYPE_COMMAND     0x03
#define PACKET_TYPE_RESERVE     0x04

// ACK
#define PACKET_NO_ACK_REQ       0
#define PACKET_ACK_REQ          1


typedef enum {
    STATE_SLEEP = 0,
    STATE_TRX_OFF,
    STATE_PLL_ON,
    STATE_RX_ON,
    STATE_RX_AACK_ON,
    STATE_TX_ARET_ON,
    STATE_BUSY_TX_ARET,
} RadioState;


/*-----------------------------------------------------------------------------
 *          Static Variables
-----------------------------------------------------------------------------*/

volatile char current_state_;
static unsigned char radio_lqi_[5];

static MacPacket mac_rx_packet_;
static MacPacket mac_tx_packet_;

static PayQueue tx_queue_;
static PayQueue rx_queue_;


/*-----------------------------------------------------------------------------
 *      Declaration of static functions (Transceiver-specific functions)
-----------------------------------------------------------------------------*/
static void trxHandleISR(void);
static void trxReceivePacket(void);
static void trxSendPacket(void);
static MacPacket trxCreateMacPacket(void);
static inline void trxSetSlptr(char val);
static unsigned char trxReadReg(unsigned char addr);
static void trxWriteReg(unsigned char addr, unsigned char val);
static unsigned char trxReadBit(unsigned char addr, unsigned char mask, 
                    unsigned char pos);
static void trxWriteBit(unsigned char addr, unsigned char mask, 
            unsigned char pos, unsigned char val);
//static unsigned char trxReadSram(unsigned char addr, unsigned char length);
//static void trxWriteSram(unsigned char addr, unsigned char* data);
static unsigned char trxReadByte(void);
static unsigned char trxWriteByte(unsigned char dout);
static void trxSetup(void);
static void trxSetupPeripheral(void);


/*-----------------------------------------------------------------------------
 *          Public functions
-----------------------------------------------------------------------------*/

void radioSetup(unsigned int tx_queue_length, unsigned int rx_queue_length) {

    trxSetup();
   
    current_state_ = STATE_RX_AACK_ON;

    mac_rx_packet_ = trxCreateMacPacket();

    mac_tx_packet_ = trxCreateMacPacket();

    tx_queue_ = pqInit(tx_queue_length);

    rx_queue_ = pqInit(rx_queue_length);

    ConfigINT4(RISING_EDGE_INT & EXT_INT_ENABLE & EXT_INT_PRI_5); // Radio    
    
}

/*****************************************************************************
* Function Name : radioReadId
* Description   : This function reads ID number of AT86RF231 chip
* Parameters    : A character array of size 4 to hold id value
* Return Value  : None
*****************************************************************************/
void radioReadTrxId(unsigned char *id) {
    SPI_CS = 1;     // just to make sure to set chip-select
    id[0] = trxReadReg(RG_PART_NUM);      // should be 3
    id[1] = trxReadReg(RG_VERSION_NUM);   // should be 2
    id[2] = trxReadReg(RG_MAN_ID_1);      // should be 0x1F
    id[3] = trxReadReg(RG_MAN_ID_0);      // should be 0

}

unsigned char radioGetTrxState(void) {
    return trxReadBit(SR_TRX_STATUS);
}


int radioIsTxQueueFull(void) {
    return pqIsFull(tx_queue_);
}

int radioIsRxQueueEmpty(void){
    return pqIsEmpty(rx_queue_);
}


void radioDeleteQueues(void) {

    while (!pqIsEmpty(tx_queue_)) {
        payDelete(pqPop(tx_queue_));
    }

    while (!pqIsEmpty(rx_queue_)) {
        payDelete(pqPop(rx_queue_));
    }

}

/*****************************************************************************
* Function Name : radioGetRxFrame
* Description   : . 
* Parameters    : None
* Return Value  : A character read from the radio.                                                     
*****************************************************************************/
Payload radioGetRxPayload(void) {

    if (pqIsEmpty(rx_queue_)) {
        return NULL;
    } else {
        return pqPop(rx_queue_);
    }
}



/*****************************************************************************
* Function Name : radioSendPayload
* Description   : transmit a payload
* Parameters    : payload to be tramsmitted.
* Optional Parameters:
*                 An integer value of destination address and/or an integer
*                 value of destination PAN ID.
*                 If any of the optional parameters are omitted, the default
*                 values are used.
* Usage         : radioTxPayload(payload) or
*                 radioTxPayload(payload, dest_address) or
*                 radioTxPayload(payload, dest_address, dest_pan_id)
* Return Value  : None                                                     
*****************************************************************************/
unsigned char radioSendPayload(Payload pay, ...) {

    /*
    va_list vargs;
    unsigned char count = 0;
    int address[2];

    va_start (vargs, pay);

    while ((addr = va_arg(argptr, int)) != NULL) {
        address[count++] = addr;
    }
    va_end (vargs);
    */

    if (pqIsFull(tx_queue_)) {
        return 0;
    } else {
        pqPush(tx_queue_, pay);
        trxSendPacket();
        return 1;
    }

}




/*****************************************************************************
* Function Name : radioGetChar
* Description   : 
* Parameters    : None
* Return Value  : 
*****************************************************************************/
unsigned char radioGetChar(unsigned char *c) {
    volatile static int data_length = 0;
    volatile static int data_loc = 0;
    static Payload pld;


    if (data_length == data_loc) {  // buffer's been all read out
        //sprintf(lcdstr, "%d", rx_queue_->size);
        //print(lcdstr);
        //delay_ms(500);
        if (pqIsEmpty(rx_queue_)) return 0;     // queue is empty
        //_LATB9 = 1;
        pld = pqPop(rx_queue_);
        data_length = payGetDataLength(pld);
        data_loc = 0;
    }

    //_LATB10 = 1;
    *c = payReadByte(pld, data_loc++);
    if (data_length == data_loc) {  // buffer's been all read out
        payDelete(pld);
    }

    return 1;

}

/*****************************************************************************
* Function Name : radioPutChar
* Description   : send out a character 
* Parameters    : c - a character to be sent out
* Return Value  : None                                                     
*****************************************************************************/
void radioPutChar(unsigned char c) {

    Payload payload = payCreate(1, &c, 0, 0);
    radioSendPayload(payload);

}




/*-----------------------------------------------------------------------------
 * The functions below have been developed for a specific radio transceiver, 
 * AT86RF231.
-----------------------------------------------------------------------------*/

/*-----------------------------------------------------------------------------
 * ----------------------------------------------------------------------------
 * The functions below are intended for internal use, i.e., private methods.
 * Users are recommended to use functions defined above.
 * ----------------------------------------------------------------------------
-----------------------------------------------------------------------------*/


/*****************************************************************************
* Function Name : _INT4Interrupt
* Description   : Interrupt hander for 802.15.4 radio
* Parameters    : None
* Return Value  : None
*****************************************************************************/
void __attribute__((interrupt, no_auto_psv)) _INT4Interrupt(void) {

    trxHandleISR();
    _INT4IF = 0;    // Clear the interrupt flag

}

/*****************************************************************************
* Function Name : trxHandleISR
* Description   : The function is called by interrupt, IRQ_TRX_END. EXT_INT4 
*                 should be enabled and RD11 should be set as input.
* Parameters    : None
* Return Value  : None
*****************************************************************************/
static void trxHandleISR(void) {
    unsigned char irq_cause;
    unsigned char trx_status;

    irq_cause = trxReadReg(RG_IRQ_STATUS);

    if (irq_cause == TRX_IRQ_TRX_END) {
        if (current_state_ == STATE_RX_AACK_ON) { // new packet arrived
            trxReceivePacket();            // receive the packet   
        } else {    // transmit is completed.
            trx_status = trxReadBit(SR_TRAC_STATUS);

            if (trx_status != TRAC_SUCCESS) {  // No ACK.
                // do something.... 
                // write codes here.....
                // LED_2 = 1;
            }

            if (pqIsEmpty(tx_queue_)) {  // tx queue is empty
                trxWriteReg(RG_TRX_STATE, CMD_PLL_ON); // change state to PLL_ON
                trxWriteReg(RG_TRX_STATE, CMD_RX_AACK_ON);  // change state to RX
                current_state_ = STATE_RX_AACK_ON;
            } else {
                current_state_ = STATE_TX_ARET_ON;
                trxSendPacket();
            }

        }
    }


}



/*****************************************************************************
* Function Name : trxSendPacket
* Description   : send out data
* Parameters    : length - number of bytes to send
*                 frame - pointer to an array of bytes to be sent. Users
*                 must define the array before this function call.
* Return Value  : None                                                     
*****************************************************************************/
static void trxSendPacket(void) {
    
    if (current_state_ ==  STATE_BUSY_TX_ARET) {
        return;
    }

    unsigned char state;
    unsigned char i;
    static unsigned char sqn = 0;


    while (1) {     // wait until radio is not busy
        state = trxReadBit(SR_TRX_STATUS);
        if (state == CMD_TX_ARET_ON) {
            break;
        } else if (state == CMD_RX_AACK_ON) {
            trxWriteReg(RG_TRX_STATE, CMD_PLL_ON); 
            trxWriteReg(RG_TRX_STATE, CMD_TX_ARET_ON); 
            break;
        }
    }

    Payload pld = pqPop(tx_queue_); 

    mac_tx_packet_->payload = pld;
    mac_tx_packet_->payloadLength = payGetPayloadLength(pld);

    current_state_ = STATE_BUSY_TX_ARET;
    trxSetSlptr(1);
    trxSetSlptr(0);

    SPI_CS = 0;     // begin SPI
    trxWriteByte(TRX_CMD_FW);


    trxWriteByte(mac_tx_packet_->payloadLength + MAC_HEADER_LENGTH + CRC_LENGTH);
    trxWriteByte(mac_tx_packet_->frame_ctrl.val.byte.LB);
    trxWriteByte(mac_tx_packet_->frame_ctrl.val.byte.HB);
    //trxWriteByte(mac_tx_packet_->seqNum);
    trxWriteByte(sqn++);
    trxWriteByte(mac_tx_packet_->destPANID.byte.LB);
    trxWriteByte(mac_tx_packet_->destPANID.byte.HB);
    trxWriteByte(mac_tx_packet_->destAddr.byte.LB);
    trxWriteByte(mac_tx_packet_->destAddr.byte.HB);
    //trxWriteByte(packet.srcPANID.byte.LB);
    //trxWriteByte(packet.srcPANID.byte.HB);
    trxWriteByte(mac_tx_packet_->srcAddr.byte.LB);
    trxWriteByte(mac_tx_packet_->srcAddr.byte.HB);

    payInitIterator(mac_tx_packet_->payload);
    for (i = 0; i < mac_tx_packet_->payloadLength; i++) {
        trxWriteByte(payNextElement(mac_tx_packet_->payload));
    }
    SPI_CS = 1; // end SPI

    payDelete(mac_tx_packet_->payload);

}

/*****************************************************************************
* Function Name : trxRecievePacket
* Description   : recieve data from radio without interrupt. 
* Parameters    : None
* Return Value  : None
*****************************************************************************/
static void trxReceivePacket(void) {

    if (!trxReadBit(SR_RX_CRC_VALID)) {  // check CRC
        return;                          // CRC invalid
    }

    unsigned char i, length;
    Payload pld;

    SPI_CS = 0;     /* Select transceiver */
    trxWriteByte(TRX_CMD_FR);
    
    length = trxReadByte() - MAC_HEADER_LENGTH - CRC_LENGTH;
    mac_rx_packet_->payloadLength = length;
    mac_rx_packet_->frame_ctrl.val.byte.LB = trxReadByte();
    mac_rx_packet_->frame_ctrl.val.byte.HB = trxReadByte();
    mac_rx_packet_->seqNum = trxReadByte();
    mac_rx_packet_->destPANID.byte.LB = trxReadByte();
    mac_rx_packet_->destPANID.byte.HB = trxReadByte();
    mac_rx_packet_->destAddr.byte.LB = trxReadByte();
    mac_rx_packet_->destAddr.byte.HB = trxReadByte();
    if (!mac_rx_packet_->frame_ctrl.bits.panIDComp) {
        mac_rx_packet_->srcPANID.byte.LB = trxReadByte();
        mac_rx_packet_->srcPANID.byte.HB = trxReadByte();
    }
    mac_rx_packet_->srcAddr.byte.LB = trxReadByte();
    mac_rx_packet_->srcAddr.byte.HB = trxReadByte();


    pld = payCreateEmpty(length-2);
    paySetStatus(pld, trxReadByte());
    paySetType(pld, trxReadByte());

    for (i = 0; i < length-2; ++i) {           
        payWriteByte(pld, i, trxReadByte());
    }

    radio_lqi_[0] = trxReadByte();

    SPI_CS = 1;     /* deselect transceiver */

    pqPush(rx_queue_, pld);

}


/*****************************************************************************
* Function Name : trxCreateMacPacket
* Description   : 
* Parameters    : 
* Return Value  : None                                                     
*****************************************************************************/
MacPacket trxCreateMacPacket(void) {

    MacPacket packet = (MacPacket)malloc(sizeof(MacPacketStruct));

    packet->frame_ctrl.bits.packetType = PACKET_TYPE_DATA;
    packet->frame_ctrl.bits.secEn = 0;
    packet->frame_ctrl.bits.frmPending = 0;
    packet->frame_ctrl.bits.ackReq = PACKET_ACK_REQ;
    packet->frame_ctrl.bits.panIDComp = 1;
    packet->frame_ctrl.bits.reserved = 0;
    packet->frame_ctrl.bits.destAddrMode = 2;
    packet->frame_ctrl.bits.frmVersion = 1;
    packet->frame_ctrl.bits.srcAddrMode = 2;
    packet->seqNum = 0;
    packet->destPANID.val = DEFAULT_DEST_PAN_ID;
    packet->srcPANID.val = DEFAULT_SRC_PAN_ID;
    packet->destAddr.val = DEFAULT_DEST_ADDR;
    packet->srcAddr.val = DEFAULT_SRC_ADDR;

    return packet;

}


/*****************************************************************************
* Function Name : trxSetSlptr
* Description   : Set the level of the SLP_TR pin.
* Parameters    : val -> 0 for LOW or 1 for HIGH level of the pin
* Return Value  : None                                                     
*****************************************************************************/
static inline void trxSetSlptr(char val) {  	
    SLPTR = val;
    Nop();
    Nop();
}


/*****************************************************************************
* Function Name : trxReadReg                                           
* Description   : Read the value from a register.
* Parameters    : addr - the offset of the register
* Return Value  : register value                                                      
*****************************************************************************/
static unsigned char trxReadReg(unsigned char addr) {
    unsigned char c;
    SPI_CS = 0;
    trxWriteByte(TRX_CMD_RR | addr);
    c = trxReadByte();
    SPI_CS = 1;
    return c;
}


/*****************************************************************************
* Function Name : trxWriteReg                                           
* Description   : Write a value to a register.
* Parameters    : addr 	 the offset of the register
*    	          val 	 the value to be written
* Return Value  : None                                                     
*****************************************************************************/
static void trxWriteReg(unsigned char addr, unsigned char val) {
    SPI_CS = 0;
    trxWriteByte(TRX_CMD_RW | addr);
    trxWriteByte(val);
    SPI_CS = 1;
}



/*****************************************************************************
* Function Name : trxReadBit                                           
* Description   : Read a bit from a register.
* Parameters    : use sub-registers defined in at86rf231.h
* Return Value  : register bit                                                      
*****************************************************************************/
static unsigned char trxReadBit(unsigned char addr,unsigned char mask, unsigned char pos) {
    unsigned char data;
    data = trxReadReg(addr);
    data &= mask;
    data >>= pos;
    return data;
}

/*****************************************************************************
* Function Name : trxWriteBit                                           
* Description   : Write a bit to a register.
* Parameters    : use sub-registers defined in at86rf231.h
* Return Value  : None                                                     
*****************************************************************************/
static void trxWriteBit(unsigned char addr, unsigned char mask, 
                unsigned char pos, unsigned char val) {
    unsigned char temp;
    temp = trxReadReg(addr);
    temp &= ~mask;
    val <<= pos;
    val &= mask;
    val |= temp;
    trxWriteReg(addr, val);
}


/******************************************************************************
* Function Name : trxReadSram                                          
* Description   : Read bytes within frame buffer
* Parameters    : addr - the first position of the bytes 
*                 length - the length of the bytes
* Return Value  :                                                   
******************************************************************************/
/*
static unsigned char trxReadSram(unsigned char addr, unsigned char length) {

    SPI_CS = 0;
    trxWriteByte(0x00);
    trxWriteByte(addr);

    // write code for frame buffer reading....
    
    SPI_CS = 1;
    return 0;

}
*/



/*****************************************************************************
* Function Name : trxWriteSram                                          
* Description   : Write bytes to frame buffer.
* Parameters    : addr - the first position of bytes in frame buffer
*    	          data - the pointer of the data to be written
* Return Value  : None                                                     
*****************************************************************************/
/*
static void trxWriteSram(unsigned char addr, unsigned char* data) {
    SPI_CS = 0;
    trxWriteByte(0x40);
    trxWriteByte(addr);

    // write code for frame buffer writing....

    SPI_CS = 1;
}
*/

/******************************************************************************
* Function Name :   trxReadByte 
* Description   :   This function will read single byte from SPI bus. 
* Parameters    :   None 
* Return Value  :   contents of SPIBUF register                           
******************************************************************************/
static unsigned char trxReadByte(void) {
    SPI_STATbits.SPIROV = 0;
    SPI_BUF = 0x00;     // initiate bus cycle
    while(SPI_STATbits.SPITBF);
    while(!SPI_STATbits.SPIRBF);
    return (SPI_BUF & 0xff);    // return byte read 
}

/******************************************************************************
* Function Name : trxWriteByte
* Description   : This routine writes a single byte to SPI bus.                                 
* Parameters    : Single data byte for SPI bus          
* Return Value  : contents of SPIBUF register
*******************************************************************************/
static unsigned char trxWriteByte(unsigned char dout) {   
    unsigned char c;
    SPI_BUF = dout;   // initiate SPI bus cycle by byte write 
    while(SPI_STATbits.SPITBF);
    while(!SPI_STATbits.SPIRBF);
    c = SPI_BUF;    // read out to avoid overflow 
    return c;
}


/******************************************************************************
* Function Name : trxSetup
* Description   : This routine initialize the transceiver.
* Parameters    : None
* Return Value  : None
*******************************************************************************/

void trxSetup(void) {

    trxSetupPeripheral();

    SPI_CS = 1;     // set chip-select

    // transition to trx_off
    trxWriteReg(RG_TRX_STATE, CMD_FORCE_TRX_OFF); 

    // interrupt at the end of frame send/receive
    trxWriteReg(RG_IRQ_MASK, TRX_IRQ_TRX_END); 

    // automatic CRC generation fro tx operation
    trxWriteBit(SR_TX_AUTO_CRC_ON, 1); 

    // no clock on clkm pin
    trxWriteBit(SR_CLKM_CTRL, CLKM_NO_CLOCK); 

    // set default radio channel 
    trxWriteBit(SR_CHANNEL, DEFAULT_CHANNEL);

    // clear any pending iterrupt
    trxReadReg(RG_IRQ_STATUS);

    // set address
    trxWriteReg(RG_SHORT_ADDR_0, (DEFAULT_SRC_ADDR & 0xff));
    trxWriteReg(RG_SHORT_ADDR_1, ((DEFAULT_SRC_ADDR >> 8) & 0xff));
    
    // set PAN ID
    trxWriteReg(RG_PAN_ID_0, (DEFAULT_SRC_PAN_ID & 0xff));
    trxWriteReg(RG_PAN_ID_1, ((DEFAULT_SRC_PAN_ID >> 8) & 0xff));

    // number of attempts until giving up sending a frame sucessfully
    trxWriteBit(SR_MAX_FRAME_RETRIES, 2);  // 3 attempts (2 retries)

    // number of max CSMA attempts until giving up sending a frame
    trxWriteBit(SR_MAX_CSMA_RETRIES, 2);  // 3 attempts (2 retries)

    // transition to rx_on
    trxWriteReg(RG_TRX_STATE,  CMD_RX_AACK_ON); 

}


/******************************************************************************
* Function Name : trxSetupPeripheral
* Description   : This routine sets up SPI bus for this module
* Parameters    : None
* Return Value  : None
*******************************************************************************/
static void trxSetupPeripheral(void) {




    // SPI interrupt is not used.
    //IFS0bits.SPI2IF = 0; // Clear the Interrupt Flag
    //IEC0bits.SPI2IE = 0; // Disable the Interrupt

    // SPI1CON1 Register Settings
    SPI_CON1bits.DISSCK = 0; // Internal Serial Clock is Enabled
    SPI_CON1bits.DISSDO = 0; // SDOx pin is controlled by the module
    SPI_CON1bits.MODE16 = 0; // Communication is byte-wide (8 bits)
    SPI_CON1bits.SMP = 0; // Input data is sampled at middle of data output time
    SPI_CON1bits.SSEN = 0; // SSx pin is used
    SPI_CON1bits.CKE = 1; // Serial output data changes on transition
                        // from active clock state to idle clock state
    SPI_CON1bits.CKP = 0; // Idle state for clock is a low level;
                            // active state is a high level
    SPI_CON1bits.MSTEN = 1; // Master mode Enabled

    // Set up SCK frequency of 6.667Mhz for 40 MIPS
    SPI_CON1bits.SPRE = 0b010; // Secondary prescale    6:1
    SPI_CON1bits.PPRE = 0b11; // Primary prescale       1:1

    // SPI2CON2 Register Settings
    SPI_CON2 = 0x0000; // Framed SPI2 support disabled

    // SPI2STAT Register Settings
    SPI_STATbits.SPISIDL = 1; // Discontinue module when device enters idle mode
    SPI_STATbits.SPIROV = 0; // Clear Overflow
    SPI_STATbits.SPIEN = 1; // Enable SPI module

}


