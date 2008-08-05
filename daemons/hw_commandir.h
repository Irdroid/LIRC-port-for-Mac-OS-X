 
/****************************************************************************
 ** hw_commandir.h **********************************************************
 ****************************************************************************
 * 
 * Copyright (C) 1999 Christoph Bartelmus <lirc@bartelmus.de>
 * -- Original hw_default.h
 * Modified for CommandIR Transceivers, April-June 2008, Matthew Bodkin 
 *
 */

#ifndef HW_COMMANDIR_H
#define HW_COMMANDIR_H

#define RX_BUFFER_SIZE 1024
#define TX_BUFFER_SIZE 1024
#define TX_QUEUE 1
#define RX_QUEUE 0
#define MAX_COMMANDIRS 4
#define MAX_COMMAND 8192

/* transmitter channel control */
#define MAX_DEVICES		4
#define MAX_CHANNELS    16
#define DEVICE_CHANNELS	4
#define MAX_MASK 		0xffff
#define MAX_SIGNALQ		100

/* CommandIR control codes */
#define CHANNEL_EN_MASK	1
#define FREQ_HEADER     2
#define MCU_CTRL_SIZE   3
#define TX_HEADER       7
#define TX_HEADER_NEW	8
/* New for CommandIR II  */

#define READ_INPUTS		10
#define PROC_SET		11
#define INIT_FUNCTION	12
#define RX_SELECT		13
#define TX_COMMANDIR_II 14
/* Internal to driver */
#define TX_LIRC_T	    15
#define FREQ_HEADER_LIRC 16
#define RXDECODE_HEADER_LIRC 17
#define INIT_HEADER_LIRC 18
#define DEINIT_HEADER_LIRC 19
#define GET_VERSION 	20

#define COMMANDIR_2_PULSE_MASK 0x8000
#define COMMANDIR_2_OVERFLOW_MASK 0x4000

#define DEFAULT_PULSE_WIDTH 13

#define USB_CMDIR_VENDOR_ID		0x10c4
#define USB_CMDIR_PRODUCT_ID	0x0003
#define USB_CMDIR_MINOR_BASE	192

#define HW_COMMANDIR_MINI 	1
#define HW_COMMANDIR_2		2
#define HW_COMMANDIR_UNKNOWN 127

#define MAX_HW_MINI_PACKET 64

// CommandIR has lots of buffer room, we don't need to poll constantly
#define USB_TIMEOUT_MS 5000
#define USB_TIMEOUT_US 1000
#define WAIT_BETWEEN_READS_US 10000
#define MAX_WAIT_BETWEEN_READS_US 5000000
#define MIN_WAIT_BETWEEN_READS_US 5000

#define USB_MAX_BUSES	8
#define USB_MAX_BUSDEV	127

#define RX_HEADER_DATA 		0x01
#define RX_HEADER_EVENTS 	0x02
#define RX_HEADER_TXAVAIL 	0x03


// We keep CommandIR's OPEN even on -deinit for speed and to monitor 
// Other non-LIRC events (plugin, suspend, etc)
#define USB_KEEP_WARM 1

// CommandIR lircd.conf event driven code definitions
#define LIRCCODE_GAP  125000
#define JACK_PLUG_1		0x01
#define JACK_PLUG_2		0x02
#define JACK_PLUG_3		0x03
#define JACK_PLUG_4		0x04
#define JACK_PLUG_5		0x11
#define JACK_PLUG_6		0x12
#define JACK_PLUG_7		0x13
#define JACK_PLUG_8		0x14
#define JACK_PLUG_9		0x21
#define JACK_PLUG_10	0x22
#define JACK_PLUG_11	0x23
#define JACK_PLUG_12	0x24
#define JACK_PLUG_13	0x31
#define JACK_PLUG_14	0x32
#define JACK_PLUG_15	0x33
#define JACK_PLUG_16	0x34

#define JACK_UNPLUG_1	0x05
#define JACK_UNPLUG_2	0x06
#define JACK_UNPLUG_3	0x07
#define JACK_UNPLUG_4	0x08
#define JACK_UNPLUG_5	0x15
#define JACK_UNPLUG_6	0x16
#define JACK_UNPLUG_7	0x17
#define JACK_UNPLUG_8	0x18
#define JACK_UNPLUG_9	0x25
#define JACK_UNPLUG_10	0x26
#define JACK_UNPLUG_11	0x27
#define JACK_UNPLUG_12	0x28
#define JACK_UNPLUG_13	0x35
#define JACK_UNPLUG_14	0x36
#define JACK_UNPLUG_15	0x37
#define JACK_UNPLUG_16	0x38

#define SELECT_TX_INTERNAL	0x09
#define SELECT_TX_ExTERNAL	0x0A

#define SELECT_TX_ON_1		0x0D
#define SELECT_TX_ON_2		0x1D
#define SELECT_TX_ON_3		0x2D
#define SELECT_TX_ON_4		0x3D

#define JACK_PLUG_RX_1		0x0B
#define JACK_UNPLUG_RX_1	0x0C
#define JACK_PLUG_RX_2		0x1B
#define JACK_UNPLUG_RX_2	0x1C
#define JACK_PLUG_RX_3		0x2B
#define JACK_UNPLUG_RX_3	0x2C
#define JACK_PLUG_RX_4		0x3B
#define JACK_UNPLUG_RX_4	0x3C

#define COMMANDIR_PLUG_1	0x41
#define COMMANDIR_PLUG_2	0x42
#define COMMANDIR_PLUG_3	0x43
#define COMMANDIR_PLUG_4	0x44

#define COMMANDIR_UNPLUG_1	0x45
#define COMMANDIR_UNPLUG_2	0x46
#define COMMANDIR_UNPLUG_3	0x47
#define COMMANDIR_UNPLUG_4	0x48

#define COMMANDIR_REORDERED	0x50
#define COMMANDIR_READY		0x51
#define COMMANDIR_STOPPED	0x52
#define COMMANDIR_POLL_FASTER	0x53
#define COMMANDIR_POLL_SLOWER	0x54

#define SETTRANSMITTERS_1	0xf0
#define SETTRANSMITTERS_2	0xf1
#define SETTRANSMITTERS_3	0xf2
#define SETTRANSMITTERS_4	0xf3
#define SETTRANSMITTERS_5	0xf4
#define SETTRANSMITTERS_6	0xf5
#define SETTRANSMITTERS_7	0xf6
#define SETTRANSMITTERS_8	0xf7
#define SETTRANSMITTERS_9	0xf8
#define SETTRANSMITTERS_10	0xf9
#define SETTRANSMITTERS_11	0xfa
#define SETTRANSMITTERS_12	0xfb
#define SETTRANSMITTERS_13	0xfc
#define SETTRANSMITTERS_14	0xfd
#define SETTRANSMITTERS_15	0xfe
#define SETTRANSMITTERS_16	0xff

// What's in a returning data packet
#define COMMANDIR_RX_EVENTS 		0x02
#define COMMANDIR_RX_DATA			0x01


#endif
