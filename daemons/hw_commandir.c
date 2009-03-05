/* CommandIR transceivers driver 0.96 CVS $Revision: 5.6 $
 * Supporting CommandIR II and CommandIR Mini (and multiple of both)
 * April-June 2008, Matthew Bodkin
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/utsname.h>

#include "hardware.h"
#include "ir_remote.h"
#include "lircd.h"
#include "receive.h"
#include "transmit.h"
#include "hw_commandir.h"
#include <usb.h>

extern struct ir_remote *repeat_remote;
extern char *progname;


/**********************************************************************
 *
 * internal function prototypes
 *
 **********************************************************************/
static int commandir_init();
static int commandir_deinit(void);
static char *commandir_rec(struct ir_remote *remotes);
static void commandir_read_loop();
static int cmdir_convert_RX(unsigned char *orig_rxbuffer);
static unsigned int get_time_value(unsigned int firstint, 
		unsigned int secondint, unsigned char overflow);
static lirc_t commandir_readdata(lirc_t timeout);
static int commandir_send(struct ir_remote *remote,struct ir_ncode *code);
static int commandir_ioctl(unsigned int cmd, void *arg);
//static void setEmitterMask(unsigned char highbyte, unsigned char lowbyte);
static void setEmitterMask(int bitmask);
static void commandir_transmit(char *buffer, int bytes, int bitmask, 
		unsigned int);
static void commandir_child_init();
static void shutdown_usb();
static void hardware_scan();
static void hardware_disconnect(int);
static void hardware_setorder();
static void raise_event(unsigned int);
static int commandir_read();
static int check_irsend_commandir(unsigned char *command);
static int commandir_decode(char *command);
static int commandir_receive_decode(struct ir_remote *remote,
                   ir_code *prep,ir_code *codep,ir_code *postp,
                   int *repeat_flagp,
                   lirc_t *min_remaining_gapp, lirc_t *max_remaining_gapp);
static void pipeline_check();
static int inline get_bit(int bitnum);
static void add_to_tx_pipeline(unsigned char *buffer, int bytes, 
		int channel_mask, unsigned int);
static void recalc_tx_available(int);
static void set_hash_mask(int channel_mask);
static int commandir2_convert_RX(unsigned short *bufferrx, 
		unsigned char numvalues);
static void cleanup_commandir_dev(int spotnum);

/**********************************************************************
 *
 * CommandIR Vars
 *
 **********************************************************************/
static int current_transmitter_mask = 0xff;
static char unsigned commandir_data_buffer[512];
static int last_mc_time = -1;	
static int commandir_rx_num = 0;


static char channels_en[MAX_DEVICES]; 
static char open_bus_hash[USB_MAX_BUSES][USB_MAX_BUSDEV];
static int tx_order[MAX_DEVICES];
static char device_count = 0;
static int mini_freq[MAX_DEVICES];
static int child_pipe_write = 0;
static char haveInited = 0;
// Fake 'commandir' remote signal values
static unsigned int signal_base[2][2] = { {100|PULSE_BIT, 200},
		 {1000|PULSE_BIT, 200} };

// Pipes to and from the child/parent
static pid_t child_pid = -1;
static int pipe_fd[2] = { -1, -1 };
static int pipe_tochild[2] = { -1, -1 };
static int tochild_read = -1, tochild_write = -1;

struct commandir_device
{
	usb_dev_handle *cmdir_udev;
	int bus;
	int busdev;
	int interface;
	int location;
	int hw_type;
	int hw_revision;
	int hw_subversion;
	unsigned char devnum;
	int endpoint_max[3];
} open_commandir_devices[4];

struct hardware hw_commandir =
{
	NULL,					 	/* default device */
	-1,                 		/* fd */
	LIRC_CAN_SET_SEND_CARRIER|
	LIRC_CAN_SEND_PULSE|
	LIRC_CAN_SET_TRANSMITTER_MASK|
	LIRC_CAN_REC_MODE2, 		
	LIRC_MODE_PULSE,			/* send_mode */ 
	LIRC_MODE_MODE2,            /* rec_mode */
	sizeof(lirc_t),        		/* code_length in BITS */
	commandir_init,       		/* init_func */
	NULL,     			  		/* config_func */
	commandir_deinit,     		/* deinit_func */
	commandir_send,				/* send_func */
 	commandir_rec,        		/* rec_func  */
	commandir_receive_decode,   /* decode_func */
	commandir_ioctl,            /* ioctl_func */
	commandir_readdata,		    /* readdata */
	"commandir"
};

/***   LIRC Interface Functions - Non-blocking parent thread
*/

static int commandir_receive_decode(struct ir_remote *remote,
                   ir_code *prep,ir_code *codep,ir_code *postp,
                   int *repeat_flagp,
                   lirc_t *min_remaining_gapp, lirc_t *max_remaining_gapp) {

	int i;
	i = receive_decode(remote,
                   prep,codep,postp,
                   repeat_flagp,
                   min_remaining_gapp, max_remaining_gapp);

	if(i > 0){
		static char rx_char[3] = {3, 0, RXDECODE_HEADER_LIRC};
		write(tochild_write, rx_char, 3);	
	}
	
	return i;
}


static int commandir_init()
{
	long fd_flags;
	if(haveInited){
		static char init_char[3] = {3, 0, INIT_HEADER_LIRC};
		write(tochild_write, init_char, 3);	
		return 1;
	}
	
	init_rec_buffer();	// LIRC's rec
	init_send_buffer();	// LIRC's send
	
	/* A separate process will be forked to read data from the USB
	 * receiver and write it to a pipe. hw.fd is set to the readable
	 * end of this pipe. */
	if (pipe(pipe_fd) != 0)
	{
		logprintf(LOG_ERR, "couldn't open pipe 1");
		return 0;
	}
	
	hw.fd = pipe_fd[0];	// the READ end of the Pipe
	
	if (pipe(pipe_tochild) != 0)
	{
		logprintf(LOG_ERR, "couldn't open pipe 1");
		return 0;
	}
	
	tochild_read = pipe_tochild[0];	// the READ end of the Pipe 
	tochild_write = pipe_tochild[1];	// the WRITE end of the Pipe 
	
	fd_flags = fcntl(pipe_tochild[0], F_GETFL);
  	if(fcntl(pipe_tochild[0], F_SETFL, fd_flags | O_NONBLOCK) == -1)
	{
		logprintf(LOG_ERR, "can't set pipe to non-blocking");
		return 0;
	}	
	
	child_pid= fork();
	if (child_pid== -1)
	{
		logprintf(LOG_ERR, "couldn't fork child process");
		return 0;
	}
	else if (child_pid== 0)
	{
		child_pipe_write = pipe_fd[1];	
		commandir_child_init();
		commandir_read_loop();
		return 0;	
	}
	haveInited = 1;
	
	logprintf(LOG_ERR, "CommandIR driver initialized");
	return 1;	
}


static int commandir_deinit(void)
{
	/* Trying something a bit new with this driver. Keeping the driver
	 * connected so in the future we can still monitor in the client */
	if(USB_KEEP_WARM && (!strncmp(progname, "lircd", 5)))
	{
		static char deinit_char[3] = {3, 0, DEINIT_HEADER_LIRC};
		write(tochild_write, deinit_char, 3);
		logprintf(LOG_ERR, "LIRC_deinit but keeping warm");
	}
	else
	{
		if (tochild_read >= 0)
		{
			if (close(tochild_read) < 0) 
			{
				logprintf(LOG_ERR, "Error closing pipe2");;
			}
			tochild_read = tochild_write = -1;
		}
		
		if(haveInited){
			// shutdown all USB
			if(child_pid > 0)
			{
				logprintf(LOG_ERR, "Closing child process");
				kill(child_pid, SIGTERM);
				waitpid(child_pid, NULL, 0);
				child_pid = -1;
				haveInited = 0;
			}
		}
		
		if (hw.fd >= 0)
		{
			if (close(hw.fd) < 0) logprintf(LOG_ERR, "Error closing pipe");
			hw.fd = -1;
		}
		
		logprintf(LOG_ERR, "commandir_deinit()");
	}
	return(1);
}

static int commandir_send(struct ir_remote *remote,struct ir_ncode *code)
{
	int length;
	lirc_t *signals;

	if(!init_send(remote,code)) {
		return 0;
	}

	length = send_buffer.wptr;
	signals = send_buffer.data;

	if (length <= 0 || signals == NULL) {
		return 0;
	}
	
	int cmdir_cnt =0;
	char cmdir_char[70];
	
	// Set the frequency of the signal along with the signal + transmitters
	cmdir_char[0] = 7;
	cmdir_char[1] = 0;

	cmdir_char[2] = FREQ_HEADER_LIRC;
	cmdir_char[3] = (remote->freq >> 24) & (0xff);
	cmdir_char[4] = (remote->freq >> 16) & (0xff);
	cmdir_char[5] = (remote->freq >> 8) & (0xff);
	cmdir_char[6] = (remote->freq & 0xff);
	
	write(tochild_write, cmdir_char, cmdir_char[0]);

	cmdir_cnt = 3;
	
	unsigned char * send_signals = malloc(sizeof(signals) * length + 4);
	
	send_signals[0] = (sizeof(lirc_t) * length + 4) & 0xff;
	send_signals[1] = ((sizeof(lirc_t) * length + 4) >> 8) & 0xff;
	
	send_signals[2] = TX_LIRC_T;
	send_signals[3] = (char)current_transmitter_mask;	
	
	memcpy(&send_signals[4], signals, sizeof(lirc_t) * length);
	
	if(write(tochild_write, send_signals, 
		send_signals[0] + send_signals[1] * 256) < 0)
	{
		logprintf(LOG_ERR, "Error writing to child_write");
	}
	
	free(send_signals);
	return(length);
}

static char *commandir_rec(struct ir_remote *remotes)
{
	char * returnit;
	if (!clear_rec_buffer()) return NULL;
	returnit = decode_all(remotes);
	return returnit;
}

static int commandir_ioctl(unsigned int cmd, void *arg)
{
	unsigned int ivalue;
	char cmdir_char[5];
	
	switch(cmd)
	{
	case LIRC_SET_TRANSMITTER_MASK:
		
		ivalue=*(unsigned int*)arg;
		
		if(ivalue >= MAX_MASK) return (MAX_CHANNELS);
		
		/* Support the old way of setting the frequency of the signal along 
		 * with the signal + transmitters */
		cmdir_char[0] = 5;
		cmdir_char[1] = 0;
		cmdir_char[2] = CHANNEL_EN_MASK;
		cmdir_char[3] = (unsigned char)(ivalue & 0x00FF);	// Low bits
		cmdir_char[4] = (unsigned char)(ivalue >> 8);		// High bits
		
		write(tochild_write, cmdir_char, cmdir_char[0]);

		return (0);
		
	default:
		logprintf(LOG_ERR, "Unknown ioctl - %d", cmd);
		return(-1);
	}
	
	return 1;

}

static lirc_t commandir_readdata(lirc_t timeout)
{
	lirc_t code = 0;

	if (!waitfordata(timeout))
		return 0;

	/* if we failed to get data return 0 */
	if (read(hw.fd, &code, sizeof(lirc_t)) <= 0)
		commandir_deinit();
	return code;
}

/***  End of parent fork / LIRC accessible functions  */















/***  CommandIR Client Process Functions (Handle all USB operations)
 */
 
int channels_space_available[MAX_CHANNELS]; 
int channels_space_updated = 0; // last updated time

char * signalq[MAX_SIGNALQ];	// how many signals can be queued
int signalq_len[MAX_SIGNALQ];
int signalq_bitmask[MAX_SIGNALQ];
unsigned int signalq_frequency[MAX_SIGNALQ];

int top_signalq = -1;
int next_signalq_per_channel[MAX_CHANNELS];	

unsigned char commandir_tx_start[MAX_CHANNELS*4];
unsigned char commandir_tx_end[MAX_CHANNELS*4];
unsigned char commandir_tx_available[MAX_CHANNELS];
unsigned char lastSendSignalID[MAX_CHANNELS];
unsigned char commandir_last_signal_id[MAX_CHANNELS];


// Global variables to convert channel masks to consistant easier formats
unsigned char hash_mask[MAX_CHANNELS];
unsigned char selected_channels[MAX_CHANNELS];
unsigned char total_selected_channels = 0;
int shutdown_pending = 0;
int read_delay = WAIT_BETWEEN_READS_US;
int insert_fast_zeros = 0;

int rx_hold = 0;

// This is the only one for pre-pipelinging
int pre_pipeline_emitter_mask = 0x000f; // default tx on only first CommandIR



static void pipeline_check()
{
	/* Transmit from the pipeline if it's time and there's space
	 * what's available should now be updated in the main_loop
	 */
	
	int i,j,k;
	
	i=0;
	if(top_signalq < 0) return;
	
	while(i <= top_signalq)
	{

		// Are ALL the channels this signal should TX on currently available?
		int oktosend = 1;
		set_hash_mask( signalq_bitmask[ i ] );
		for(j = 0; j<total_selected_channels; j++)
		{
			if(commandir_tx_available[ selected_channels[j] ] < 
				(36 + (signalq_len[ i ])/sizeof(lirc_t)))	
			{
				oktosend = 0;
				break;
			}
		}
		
		if(oktosend)
		{
			// great, TX this on all the channels.
			
			commandir_transmit(signalq[ i ], signalq_len[ i ], signalq_bitmask[ i ], signalq_frequency[ i ]);
			
			for(j = 0; j<total_selected_channels; j++)
			{
				/*  commandir_tx_available[ selected_channels[j] ] -= 
					(64 + (signalq_len[ i ])/sizeof(lirc_t));  */
				commandir_tx_available[ selected_channels[j] ] = 0; 
			}
			
			/* Free up the memory, and see if there are new next_signalq's 
			 * (any more for this channel to TX)
			 */
			free(signalq[i]);
			
			for(k=i; k<top_signalq; k++)
			{
				signalq[k] = signalq[k+1];
				signalq_len[k] = signalq_len[k+1];
				signalq_bitmask[k] = signalq_bitmask[k+1];
				signalq_frequency[k] = signalq_frequency[k+1];
			}
			top_signalq--;
		}
		else
		{
			i++;
		}
	}
}

static int get_bit(int bitnum)
{
	int r = 1;
	return r << bitnum; // bit 0 is 1, bit 1 is 10, bit 2 is 100...
}

static void add_to_tx_pipeline(unsigned char *buffer, int bytes, 
	int channel_mask, unsigned int frequency)
{
	/* *buffer points to a buffer that will be OVERWRITTEN; malloc our copy.
	 * buffer is a LIRC_T packet for CommandIR 
	 */
	top_signalq++;
	if(top_signalq > MAX_SIGNALQ)
	{
		logprintf(LOG_ERR, "Too many signals in queue: > %d", MAX_SIGNALQ);
		return;
	}
	
	signalq[top_signalq] = malloc(bytes);
	
	signalq_len[top_signalq] = bytes;
	signalq_bitmask[top_signalq] = channel_mask;
	signalq_frequency[top_signalq] = frequency;
	
	lirc_t *oldsignal, *newsignal;
	int x, pulse_now = 1;
	int projected_signal_length;
	short aPCAFOM = 0;
	float afPCAFOM = 0.0;
	int difference = 0;
					
	afPCAFOM = (6000000.0 / ((frequency > 0) ? frequency : DEFAULT_FREQ)  ) ; 
	aPCAFOM = afPCAFOM;
	
	// Trim off mid-modulation pulse fragments, add to space for exact signals
	for(x=0; x<(bytes/sizeof(lirc_t)); x++)
	{
		oldsignal = (lirc_t *)&buffer[x*sizeof(lirc_t)];
		newsignal = (lirc_t *)signalq[top_signalq];
		newsignal += x;
		
		if(pulse_now==1){
			projected_signal_length =  
				(((int)( (*oldsignal * 12)/( afPCAFOM ) ))  * aPCAFOM) / 12;
			difference = *oldsignal - projected_signal_length;
			// take off difference plus 1 full FOM cycle
			*newsignal = *oldsignal - difference - (aPCAFOM / 12);	
			
		}
		else
		{
			if(difference != 0)
			{
				// Add anything subtracted from the pulse to the space
				*newsignal = *oldsignal + difference + (aPCAFOM / 12);
				difference = 0;
			}
		
		}
		
		pulse_now++;
		if(pulse_now > 1) pulse_now = 0;
	}
	
	return;
}

static void recalc_tx_available(int which_commandir)
{
	int i;
	int length = 0;
	static int failsafe = 0;
	
	if(lastSendSignalID[which_commandir] != 
		commandir_last_signal_id[which_commandir])
	{
		/* INNOVATIONONE_FLAG:REMOVE  This will be removed pending testing
		 * for a future release
		 */
		if(failsafe++ < 1000)
		{
			return;
		}
		logprintf(LOG_ERR, "Error: required the failsafe");
	}
	
	failsafe = 0;
	for(i=which_commandir*4; i<((which_commandir+1)*4); i++)
	{
		length = commandir_tx_end[i] - commandir_tx_start[i];
		if(length < 0) length += 0xff;
		
		if(commandir_tx_available[i] < 0xff - length)
			commandir_tx_available[i] = 0xff - length;
			
	}
}

static void set_hash_mask(int channel_mask) // eg, 8
{
	// bitwise set of hash_mask for easier coding...
	int i,j;
	j=channel_mask;
	total_selected_channels = 0;
	for(i=0; i<MAX_CHANNELS; i++)
	{
		hash_mask[i] = j & 0x01;
		j = j >> 1;
		if(hash_mask[i])
			selected_channels[total_selected_channels++] = i;
	}
}


static void commandir_transmit(char *buffer, int bytes, int bitmask, 
	unsigned int frequency)
{
	/*** Send a TX command to 1 or more CommandIRs.
	 * Keep in mind: TX frequency, TX channels, TX signal length, 
	 * which CommandIR, & what hardware version
	 */
	
	int send_status;
	unsigned char packet[TX_BUFFER_SIZE];
	/* So we know where there should be gaps between signals and more 
	 * importantly, where there shouldn't be
	 */
	static char signalid = 1;	
	
	/* Depending on the tx channels, then depending on what hardware it is, 
	 * set the freq if needed, and send the buffer with the channel header 
	 * that's right for that CommandIR
	 */
	
	int devicenum = 0;
	int sent = 0, tosend = 0;
	unsigned char mini_tx_mask = 0;
	lirc_t * signals;	// have bytes/sizeof(lirc_t) signals
	signals = (lirc_t *)buffer;
	int total_signals = 0;
	int i;
	char cmdir_char[66];
	int which_signal = 0;
	
	total_signals = bytes / sizeof(lirc_t);
	
	setEmitterMask(bitmask);
	
	for(devicenum = 0; devicenum < device_count; devicenum++)
	{
		// Do we transmit on any channels on this device?
		if(channels_en[ devicenum ])
		{
			which_signal = 0;
			switch(open_commandir_devices[ tx_order[devicenum] ].hw_type)
			{
				case HW_COMMANDIR_2:
					
					mini_tx_mask = 0;
					// Short enough loop to unroll
					if(channels_en[ devicenum ] & 1) mini_tx_mask |= 0x10;
					if(channels_en[ devicenum ] & 2) mini_tx_mask |= 0x20;
					if(channels_en[ devicenum ] & 4) mini_tx_mask |= 0x40;
					if(channels_en[ devicenum ] & 8) mini_tx_mask |= 0x80;
					
					packet[1] = TX_COMMANDIR_II;
					packet[2] = mini_tx_mask;
					
					short PCAFOM = 0;
					float fPCAFOM = 0.0;
					
					if(bytes/sizeof(lirc_t) > 255)
					{
						logprintf(LOG_ERR, "Error: signal over max size");
						continue; 
					}
					
					fPCAFOM = (6000000 / ((frequency > 0) ? frequency : 
						DEFAULT_FREQ)  ) ; 
					PCAFOM = fPCAFOM;
					
					lastSendSignalID[  tx_order[devicenum]   ] = packet[5] = (getpid() + signalid++) + 1;	
					
					packet[4] = PCAFOM & 0xff;
					packet[3] = (PCAFOM >> 8) & 0xff;					
					
					short packlets_to_send = 0, sending_this_time = 0;
					
					packlets_to_send = bytes / sizeof(lirc_t);
					
					int attempts;
					for(attempts = 0; attempts < 10; attempts++)
					{
					
						if((packlets_to_send*3 + 7) > open_commandir_devices[ tx_order[devicenum] ].endpoint_max[1])
						{
							sending_this_time = open_commandir_devices[ tx_order[devicenum] ].endpoint_max[1]/3 - 3;
						}
						else
						{
							sending_this_time = packlets_to_send;
						}
						int sending;

						for(i=0; i<sending_this_time; i++)
						{
							sending = signals[which_signal++];
							
							packet[i*3+7] = sending >> 8; // high1
							packet[i*3+8] = sending & 0xff; // low
							packet[i*3+9] = sending >> 16 & 0xff; // high2
						}
						
						packet[0] = (sending_this_time * 3 + 7);
						packet[6] = (sending_this_time == packlets_to_send)  ? 0xcb :  0x00;
						
						send_status=usb_bulk_write(
							open_commandir_devices[ tx_order[devicenum] ].cmdir_udev, 
							2, // endpoint2
							(char*)packet,
							packet[0], 
							USB_TIMEOUT_MS);
						if(send_status < 0)
						{
							// Error transmitting.
							hardware_scan();
							return;
						}
						
						packlets_to_send -= ((send_status - 7) / 3);
						if(!packlets_to_send)
						{
							// "No more packlets to send\n"
							break;
						}
					}
					continue; // for transmitting on next CommandIR device
					
			
				case HW_COMMANDIR_MINI:
					mini_tx_mask = 0;
					if(channels_en[ devicenum ] & 1) mini_tx_mask |= 0x80;
					if(channels_en[ devicenum ] & 2) mini_tx_mask |= 0x40;
					if(channels_en[ devicenum ] & 4) mini_tx_mask |= 0x20;
					if(channels_en[ devicenum ] & 8) mini_tx_mask |= 0x10;
					
					char freqPulseWidth = DEFAULT_PULSE_WIDTH;
					
					freqPulseWidth = (unsigned char)((1000000 / 
						((frequency > 0) ? frequency: DEFAULT_FREQ)  ) / 2);
					
					if(freqPulseWidth == 0)
					{
						freqPulseWidth = DEFAULT_PULSE_WIDTH;
					}
					
					if(mini_freq[ tx_order[devicenum] ] != freqPulseWidth)
					{
						// Update the CommandIR Mini's next tx frequency
						cmdir_char[0] = FREQ_HEADER;
						cmdir_char[1] = freqPulseWidth;
						cmdir_char[2] = 0;
						mini_freq[ tx_order[devicenum] ] = freqPulseWidth;
						send_status=usb_bulk_write(
							open_commandir_devices[ tx_order[devicenum] ].cmdir_udev, 
							2, // endpoint2
							cmdir_char,
							2, // 2 bytes
							USB_TIMEOUT_MS);
						if(send_status < 2)
						{
							// Error transmitting.
							hardware_scan();
							return ;
						}
 					}
					
					unsigned int mod_signal_length=0;
					
					cmdir_char[0] = TX_HEADER_NEW;
 					cmdir_char[1] = mini_tx_mask;
					
					unsigned int hibyte, lobyte;

					sent = 0;
					which_signal = 0;
					while(sent < (bytes / sizeof(lirc_t) * 2 ) )
					{
 						tosend = (bytes / sizeof(lirc_t) * 2 ) - sent;
						
						if(tosend > (MAX_HW_MINI_PACKET - 2))
						{
							tosend = MAX_HW_MINI_PACKET - 2;
						}
						
						for(i=0;i<(tosend/2);i++) // 2 bytes per CommandIR pkt
						{
							mod_signal_length = signals[which_signal++] >> 3;
							hibyte = mod_signal_length/256;
							lobyte = mod_signal_length%256;
							cmdir_char[i*2+3] = lobyte;
							cmdir_char[i*2+2] = hibyte;
						}

						send_status=usb_bulk_write(
							open_commandir_devices[ tx_order[devicenum] ].cmdir_udev, 
							2, // endpoint2
							cmdir_char,
							tosend + 2, 
							USB_TIMEOUT_MS);
						if(send_status < 1)
						{
							// Error transmitting.
							hardware_scan();
							return;
						}
						sent += tosend;
					} // while unsent data
					continue; // for transmitting on next CommandIR device
				default:
					logprintf(LOG_ERR, "Unknown hardware: %d", 
						open_commandir_devices[tx_order[devicenum]].hw_type);
			} // hardware switch()
		} // if we should tx on this device
	} // for each device we have
}


static void commandir_child_init()
{
	alarm(0);
	signal(SIGTERM, shutdown_usb);
	signal(SIGPIPE, SIG_DFL);
	signal(SIGINT, shutdown_usb);
	signal(SIGHUP, SIG_IGN);
	signal(SIGALRM, SIG_IGN);
	
	logprintf(LOG_ERR, "Child Initializing CommandIR Hardware");
	
	usb_init();
	int i;
	for(i=0;i<MAX_CHANNELS;i++)
	{
		next_signalq_per_channel[i] = -1;
		channels_en[i] = 0xff;
	}
	/* Placeholder for fast decode support */
	hardware_scan();
}

static void hardware_disconnect(int commandir_spot)
{
	/* We had a read/write error, try disconnecting it and force _scan to 
	 * reconnect - otherwise we may get perpetual read/write errors
	 */
	
	int x;
	
	//	reset the hash so we don't try and disconnect this device again
	//  device_count is decremented here
	cleanup_commandir_dev(commandir_spot);
	
	raise_event(COMMANDIR_UNPLUG_1 + commandir_spot);
	
	/* Cases are:
	removing device 0 when there's no more device (do nothing)
	removing device < MAX when there's still 1+ devices (patch up)
	removing device==MAX when there's more devices (do nothing)
	*/
	
	// new device count-- from cleanup (if device_count > 0 AND commandir removed isn't 0 or max)
	if( (device_count > 0) && (commandir_spot != device_count) )
	{
		/* It wasn't the top device removed, and there's 
			* more than 1 device, so we have some vars to patch up
			*/
		for(x=commandir_spot; x<(device_count); x++)
		{
			channels_en[x] = channels_en[x+1];
			mini_freq[x] = mini_freq[x+1];
			commandir_last_signal_id[x] = commandir_last_signal_id[x+1];
			lastSendSignalID[x] = lastSendSignalID[x+1];
			memcpy(&open_commandir_devices[x], 
				&open_commandir_devices[x+1], 
				sizeof(struct commandir_device));	
		}
	
	// Reset the TOP one that was just removed:
		channels_en[(int)device_count] = 0x0f;
		mini_freq[(int)device_count] = -1;
		commandir_last_signal_id[(int)device_count] = 0;
		lastSendSignalID[(int)device_count] = 0;
		open_commandir_devices[(int)device_count].cmdir_udev = 0;
		open_commandir_devices[(int)device_count].bus = 0;
		open_commandir_devices[(int)device_count].busdev = 0;
		open_commandir_devices[(int)device_count].interface = 0; 
		open_commandir_devices[(int)device_count].hw_type = 0;
		open_commandir_devices[(int)device_count].hw_revision = 0;
		open_commandir_devices[(int)device_count].hw_subversion = 0;
		
	}
	
	if(commandir_rx_num>=commandir_spot)
	{
		commandir_rx_num--;	
	}

	hardware_setorder();
}

static void hardware_setorder(){
	/* Tried to order to the detected CommandIRs based on bus and dev ids
		* so they remain the same on reboot.  Adding a new device in front
		* will mean it becomes device 0 and emitters or scripts must be fixed
		* Need a different param, these still change. 
		*/
		
	tx_order[0] = tx_order[1] = tx_order[2] = tx_order[3] = 0; 
	mini_freq[0] = mini_freq[1] = mini_freq[2] = mini_freq[3] = -1;
	int largest = 0;
	int tmpvals[4];
	int x, tx_spots, find_spot;
	
	for(x=0; x<device_count; x++)
	{
		tmpvals[x] = 256 * open_commandir_devices[x].bus + 
			open_commandir_devices[x].busdev;
	}
	
	for(tx_spots = 0; tx_spots < device_count; tx_spots++)
	{
		largest = 0;
		for(find_spot = 0; find_spot < device_count; find_spot++)
		{
			if(tmpvals[find_spot] > tmpvals[largest])
			{
				largest = find_spot;
			}
		}
		tx_order[device_count - tx_spots - 1 ] = largest;
		tmpvals[largest] = 0;
	}
	
	
	// The formerly receiving CommandIR has been unplugged
	if(commandir_rx_num < 0)
	{
		if(device_count > 0)
			commandir_rx_num = 0;
	}
	
	// Clear all pending signals
	for(x=top_signalq; x >= 0; x--)
	{
		free(signalq[top_signalq]);
	}
	top_signalq = -1;
	
}

static void cleanup_commandir_dev(int spotnum)
{
	int location, devnum;

	location = open_commandir_devices[spotnum].location;
	devnum = open_commandir_devices[spotnum].devnum;

  open_bus_hash[ location ][ devnum ] = 0;
	device_count--;	
  
	if(open_commandir_devices[spotnum].cmdir_udev==NULL)
	{
		return;
	}
	usb_release_interface(open_commandir_devices[spotnum].cmdir_udev, 
		open_commandir_devices[spotnum].interface);
	usb_close(open_commandir_devices[spotnum].cmdir_udev);
	open_commandir_devices[spotnum].cmdir_udev = NULL;
}


static void hardware_scan()
{
	// Scan for hardware changes; libusb doesn't notify us...
	unsigned char located = 0;
	struct usb_bus *bus = 0;
	struct usb_device *dev = 0;
	
	int scan_find[MAX_DEVICES][2]; // [0]=bus#, [1]=busdev#
	unsigned char found = 0;
	// Using hash for performance instead of memory conservation (+1k)
	unsigned char still_found[USB_MAX_BUSES][USB_MAX_BUSDEV];
	unsigned changed = 0;
	int find_spot = 0;
	
	usb_find_busses();
	usb_find_devices();
 	
 	for (bus = usb_busses; bus; bus = bus->next)
	{
		for (dev = bus->devices; dev; dev = dev->next)	
		{
			if (dev->descriptor.idVendor == USB_CMDIR_VENDOR_ID) 
			{
				located++;
				// Do we already know about it?
				if(!open_bus_hash[bus->location][dev->devnum]){
				  // Then it's new, open it if we have a spot available
				  for(find_spot=0; find_spot < MAX_DEVICES; find_spot++)
				  {
					if(open_commandir_devices[find_spot].cmdir_udev == NULL)
					{
					  // Try to open here
					  open_commandir_devices[find_spot].cmdir_udev = usb_open(dev);
					  if(open_commandir_devices[find_spot].cmdir_udev == NULL)
					  {
						logprintf(LOG_ERR, 
						  "Error opening commandir - bus %d, device %d.",
						  bus, dev);
						  break;
						}
						else 
						{
						  
						// Try to set configuration; not needed on Linux
// 						int usb_set_configuration(usb_dev_handle *dev, int configuration);
 							int r = 0;
// 							r = usb_set_configuration(open_commandir_devices[find_spot].cmdir_udev, 1);
							
// 						printf("Set_configuration returned %d.\n", r);
						  
						  r = usb_claim_interface(
						  	open_commandir_devices[find_spot].cmdir_udev,0);
						  if(r < 0)
						  {
						  	cleanup_commandir_dev(find_spot);
								logprintf(LOG_ERR, 
								"Unable to claim CommandIR - Is it already busy?"
								);
								logprintf(LOG_ERR, 
								"Try 'rmmod commandir' or check for other lircds"
								);
								break;
						  }
						  else 
						  {
							// great, it's ours
							open_commandir_devices[find_spot].location = bus->location;
							open_commandir_devices[find_spot].devnum = dev->devnum;
							open_bus_hash[bus->location][dev->devnum] = 1;
							open_commandir_devices[find_spot].bus = bus->location;
							open_commandir_devices[find_spot].busdev = dev->devnum;
							scan_find[++found][0] = bus->location;
							scan_find[found][1] = dev->devnum;
							device_count++;
							changed++;
							still_found[bus->location][dev->devnum] = 1;	
							 
							struct usb_config_descriptor *config = &dev->config[0];
							struct usb_interface *interface = &config->interface[0];
							struct usb_interface_descriptor *ainterface = &interface->altsetting[0];
/*						struct usb_endpoint_descriptor *endpoint = &ainterface->endpoint[2];*/
							
							int i;// Load wMaxPacketSize for each endpoint; subtract 0x80 
										// for double-buffer bit
							for (i = 0; i < ainterface->bNumEndpoints; i++)
							{
								open_commandir_devices[find_spot].endpoint_max[ 
									(ainterface->endpoint[i].bEndpointAddress >= 0x80) 
									? (ainterface->endpoint[i].bEndpointAddress-0x80) 
									: (ainterface->endpoint[i].bEndpointAddress)] 
									= ainterface->endpoint[i].wMaxPacketSize;
							}
							
							// compensate for double buffer:
							open_commandir_devices[find_spot].endpoint_max[1] *= 2;
							
							// Always use the latest to RX:
							commandir_rx_num = find_spot;	
							
							switch(dev->descriptor.iProduct)
							{
							 case 2:
							 	logprintf(LOG_ERR, "Product identified as CommandIR II");
							 	open_commandir_devices[find_spot].hw_type = HW_COMMANDIR_2;
							  open_commandir_devices[find_spot].hw_revision = 0;
							  open_commandir_devices[find_spot].hw_subversion = 0;
							  
							  int send_status = 0, tries=20;
							  static char get_version[] = {2, GET_VERSION};

								send_status = 4;	// just to start the while()
								
								while(tries--){
									usleep(USB_TIMEOUT_US);	// wait a moment
										
									// try moving this below:
									send_status = usb_bulk_write(
									open_commandir_devices[find_spot].cmdir_udev, 
										2, // endpoint2
										get_version,
										2, 
										1500);
									if(send_status < 0)
									{
										logprintf(LOG_ERR, 
											"Unable to write version request - Is CommandIR busy? Error %d", send_status);
										break;
									}
									
									send_status = usb_bulk_read(
										open_commandir_devices[find_spot].cmdir_udev,
										1,
										(char *)commandir_data_buffer,
										open_commandir_devices[ find_spot ].endpoint_max[1],
										1500);
	
									if(send_status < 0)
									{
										logprintf(LOG_ERR, 
											"Unable to read version request - Is CommandIR busy? Error %d", send_status);
										cleanup_commandir_dev(find_spot);
										break;
									}
									if(send_status==3)
									{
										if(commandir_data_buffer[0]==GET_VERSION)
										{
											// Sending back version information.
											open_commandir_devices[find_spot].hw_revision = 
												commandir_data_buffer[1];
											open_commandir_devices[find_spot].hw_subversion = 
												commandir_data_buffer[2];
											logprintf(LOG_ERR, "Hardware revision is %d.%d.", 
												commandir_data_buffer[1], commandir_data_buffer[2]);
											break;
										}
										else
										{
											continue;
										}
									}
									
							}
							break;
							default:
								logprintf(LOG_ERR, "Product identified as CommandIR Mini");
   							open_commandir_devices[find_spot].hw_type = 
								HW_COMMANDIR_MINI;
							}
							
							if(open_commandir_devices[find_spot].hw_type == 
								HW_COMMANDIR_UNKNOWN)
							{
								logprintf(LOG_ERR, "Product UNKNOWN - cleanup");
								cleanup_commandir_dev(find_spot);
							}
							else
							{
								lastSendSignalID[find_spot] = 0;
								commandir_last_signal_id[find_spot] = 0;
							}
							break; // don't keep looping through find_spot
							} // claim?
						}// open?
					}// spot available?
				 }// for(spots)
				} // if we haven't seen it before
				else
				{
					still_found[bus->location][dev->devnum] = 1;
				}
			}// if it's a CommandIR
		}// for bus dev's
	}// for bus's
	
	if(!located)
	{
		logprintf(LOG_ERR, "No CommandIRs found");
	}
	
	/* Check if any we currently know about have been removed
	 * (Usually, we get a read/write error first)
	 */
	for(find_spot = 0; find_spot < MAX_DEVICES; find_spot++)
	{
		if(open_commandir_devices[find_spot].cmdir_udev != NULL)
		{
			if(still_found[open_commandir_devices[find_spot].location]
				[open_commandir_devices[find_spot].devnum] != 1)
			{
				logprintf(LOG_ERR, "Commandir %d removed from [%d][%d].", 
					find_spot,open_commandir_devices[find_spot].location, 
					open_commandir_devices[find_spot].devnum);
				raise_event(COMMANDIR_UNPLUG_1 + find_spot);
				hardware_disconnect(find_spot);
				commandir_rx_num = -1;
				changed++;
			}
		}
	}
	
	if(changed)
	{
		hardware_setorder();
		raise_event(COMMANDIR_REORDERED);
	}
	
}


// Shutdown everything and terminate 
static void shutdown_usb()
{
	int x;
	
	// Wait for any TX to complete before shutting down
	if(top_signalq >= 0)
	{
		shutdown_pending++;
		logprintf(LOG_ERR, "Waiting for signals to finish transmitting before shutdown");
		return;
	}
	
	for(x=0; x<MAX_DEVICES; x++)
	{
		if(open_commandir_devices[x].cmdir_udev )
		{
			usb_release_interface(open_commandir_devices[x].cmdir_udev, 
				open_commandir_devices[x].interface);
			usb_close(open_commandir_devices[x].cmdir_udev);
		}
	}
	logprintf(LOG_ERR, "CommandIR driver child cleaned up and exiting");
	raise_event(COMMANDIR_STOPPED);

	_exit(EXIT_SUCCESS);
}

static void commandir_read_loop()
{
	// Read from CommandIR, Write to pipe
	
	unsigned char commands[MAX_COMMAND];
	int curCommandStart = 0;
	int curCommandLength = 0;
	int bytes_read;
	unsigned char periodic_checks = 0;
	static unsigned char rx_decode_led[7] = {7, PROC_SET, 0x40, 0, 0,4, 2}; 
	static unsigned char init_led[7] = {7, PROC_SET, 0x00, 0x01, 3, 55, 2}; 
	static unsigned char deinit_led[7] = {7, PROC_SET, 0x0, 0x02, 3, 45, 2}; 
	static unsigned int LIRC_frequency = 38000; 

	int send_status = 0; 
	int i = 0;
	int tmp = 0;
	int tmp2 = 0;

	raise_event(COMMANDIR_READY);
	
	for(;;){
		/*** This is the main loop the monitors control and TX events from 
		  * the parent, and monitors the CommandIR RX buffer
		  */
		
		curCommandStart = 0;
		curCommandLength = 0;
		bytes_read = read(tochild_read, commands, MAX_COMMAND); 
		
		if(shutdown_pending > 0 && (top_signalq==-1))
			shutdown_usb();
		
		if(bytes_read > 0){
		
			while(curCommandStart < bytes_read){
				curCommandLength = commands[curCommandStart] + 
					commands[curCommandStart + 1] * 256;
				
				switch(commands[curCommandStart + 2]){	// the control value
					case DEINIT_HEADER_LIRC:
						for(i=0; i<device_count; i++)
						{
						 if(open_commandir_devices[tx_order[i]].hw_type ==
						  HW_COMMANDIR_2)
						 {
						  if(open_commandir_devices[tx_order[i]].cmdir_udev > 0)
						  {
							send_status=usb_bulk_write(
								open_commandir_devices[tx_order[i]].cmdir_udev, 
								2, // endpoint2
								(char*)deinit_led,
								7, // bytes
								USB_TIMEOUT_MS);
						  }
						  rx_hold = 1;	// Put a hold on RX, but queue events
						 }
						}
						
						break;
					case INIT_HEADER_LIRC:
						for(i=0; i<device_count; i++)
						{
						 if(open_commandir_devices[tx_order[i]].hw_type == 
							HW_COMMANDIR_2)
						 {
						 if(open_commandir_devices[tx_order[i] ].cmdir_udev > 0)
						  {
							send_status=usb_bulk_write(
								open_commandir_devices[tx_order[i] ].cmdir_udev,
								2, // endpoint2
								(char*)init_led,
								7, // bytes
								USB_TIMEOUT_MS);
						  }
						  rx_hold = 0;	// Resume RX after queue events
						 }
						}
						break;
					case RXDECODE_HEADER_LIRC:
					  //	Successful RX decode: show it on the light.
					  if(open_commandir_devices[commandir_rx_num].cmdir_udev > 0)
					  {
						send_status=usb_bulk_write(
							open_commandir_devices[commandir_rx_num].cmdir_udev,
							2, // endpoint2
							(char*)rx_decode_led,
							7, // bytes
							USB_TIMEOUT_MS);

					  }
					  break;

					case FREQ_HEADER_LIRC:
						LIRC_frequency = (commands[curCommandStart + 6] & 0x000000ff) | 
							((commands[curCommandStart + 5] << 8) & 0x0000ff00) | 
							((commands[curCommandStart + 4] << 16) & 0x00ff0000) | 
							((commands[curCommandStart + 3] << 24) & 0xff000000);
						if(!LIRC_frequency)
							LIRC_frequency = DEFAULT_FREQ;
						break;
					case TX_HEADER_NEW:
					case TX_LIRC_T:
						if(curCommandLength==64)
						{
							if(check_irsend_commandir(&commands[curCommandStart + 4]))
							{
								break; // it's a command for us
							}
						}
						add_to_tx_pipeline(&commands[curCommandStart + 4], 
							curCommandLength - 4, pre_pipeline_emitter_mask, LIRC_frequency);
						break;
						
					case CHANNEL_EN_MASK:
						pre_pipeline_emitter_mask = (commands[curCommandStart+4] << 8) |
							 commands[curCommandStart+3];
						break;
				}
				curCommandStart += curCommandLength;
			
			}
		}
		// If we're receiving, make sure the commandir buffer doesn't overrun
		if(commandir_read() < 20 )
			tmp = 2;
		while(tmp-- > 0)
		{
			tmp2 = commandir_read();
		}
		if(tmp2 < 20 ){
			// once in a while, but never while we're retreaving a signal
			if(++periodic_checks>100)
			{
				hardware_scan();
				periodic_checks = 0;
			}
			else
			{
 				usleep(read_delay);
 			}
		}
	}

}

static int check_irsend_commandir(unsigned char *command)
{
	// decode the code like LIRC would do, and take an action
	int commandir_code = 0;
	
	commandir_code = commandir_decode((char*)command);
	
	if(commandir_code > 0xef)
	{
		// It's a settransmitters command
		int channel = commandir_code & 0x0f;
		
		// can only set 1 bit from here so far..,
		pre_pipeline_emitter_mask = 0x0001 << channel;	
		
		return commandir_code;
	}
	
	switch(commandir_code)
	{
		case 0x53:
			read_delay /= 2;	// "faster" means less time
			if(read_delay < MIN_WAIT_BETWEEN_READS_US)
				read_delay = MIN_WAIT_BETWEEN_READS_US;
			break;
		case 0x54:
			read_delay *= 2;	// "slower" means more time
			if(read_delay > MAX_WAIT_BETWEEN_READS_US)
				read_delay = MAX_WAIT_BETWEEN_READS_US;
			break;
	
		case 0x09: 
		case 0x0A:
			logprintf(LOG_ERR, "Re-selecting RX not implemented yet");
			break;
			
		case 0xe6:	//	disable-fast-decode
			logprintf(LOG_ERR, "Fast decoding disabled");
			insert_fast_zeros = 0;
			break;
			
		case 0xe7:	//	enable-fast-decode
		case 0xe9:	//	force-fast-decode-2
			logprintf(LOG_ERR, "Fast decoding enabled");
			insert_fast_zeros = 2;
			break;
		
		case 0xe8:	//	force-fast-decode-1
			logprintf(LOG_ERR, "Fast decoding enabled (1)");
			insert_fast_zeros = 1;
			break;
			
		default:
			if(commandir_code > 0x60 && commandir_code < 0xf0)
			{
				int ledhigh = 0, ledlow = 0, ledprog = -1;
				// LED Command
				switch(commandir_code >> 4)
				{
					case 0x6: ledlow = 0x80; break;
					case 0x7: ledlow = 0x40; break;
					case 0x8: ledlow = 0x20; break;
					case 0x9: ledlow = 0x10; break;
					case 0xa: ledlow = 0x04; break;
					case 0xb: ledhigh = 0x80; break;
					case 0xc: ledlow = 0x01; break;
					case 0xd: ledlow = 0x02; break;
					case 0xe: ledlow = 0x08; break;
				}
				ledprog = (commandir_code & 0x0f) - 1;
				
				if( ((ledhigh + ledlow) > 0) && ledprog > -1)
				{
					//	Set light:
					static unsigned char lightchange[7] = {7, 
					PROC_SET, 0, 0, 0, 0, 3}; 
					lightchange[2] = ledhigh;
					lightchange[3] = ledlow;
					lightchange[4] = ledprog;
					int send_status = 0; 
					
					send_status=usb_bulk_write(
						open_commandir_devices[tx_order[0]].cmdir_udev, 
						2, // endpoint2
						(char *)lightchange,
						7, // bytes
						USB_TIMEOUT_MS);
				}
				
				return commandir_code; // done
			}
		
	}

	return commandir_code;
}


// return how many RX's were in the last receive; so we know whether to poll more frequently or not
static int commandir_read() 
{
	
	/***  Which CommandIRs do we have to read from?  Poll RX CommandIRs 
		* regularly, but non-receiving CommandIRs should be more periodically
		*/
	
	int i,j;
	int read_received = 0;
	int read_retval = 0;
	int conv_retval = 0;
	int max_read = 5;
	static int zeroterminated = 0;

	for(i=0; i<device_count; i++)
	{
	
		switch(open_commandir_devices[tx_order[i]].hw_type)
		{
			case HW_COMMANDIR_2:
		
				read_retval = usb_bulk_read(
					open_commandir_devices[ tx_order[i] ].cmdir_udev,
					1,
					(char *)commandir_data_buffer,
					open_commandir_devices[ tx_order[i] ].endpoint_max[1],
					5000);	
					
				if(read_retval==0)
					break;
				
				if(read_retval < 1)
				{
					if(read_retval < 0)
					{
						if(read_retval == -19){
							logprintf(LOG_ERR, "Read Error - CommandIR probably unplugged");
						}
						else
						{
							logprintf(LOG_ERR, 
								"Didn't receive a full packet from a CommandIR II! - err %d ."
								, read_retval);
						}
						hardware_disconnect(tx_order[i]);
						hardware_scan();
					}
					// 0 bytes is the most frequency case; nothing to report

					break;
				} 
				
				if(commandir_data_buffer[0]==RX_HEADER_TXAVAIL)
				{
					// sending us the current tx_start, tx_end arrays, and where it's at
					commandir_tx_start[tx_order[i]*4] = commandir_data_buffer[4];
					commandir_tx_start[tx_order[i]*4+1] = commandir_data_buffer[3];
					commandir_tx_start[tx_order[i]*4+2] = commandir_data_buffer[2];
					commandir_tx_start[tx_order[i]*4+3] = commandir_data_buffer[1];
					
					commandir_tx_end[tx_order[i]*4] = commandir_data_buffer[8];
					commandir_tx_end[tx_order[i]*4+1] = commandir_data_buffer[7];
					commandir_tx_end[tx_order[i]*4+2] = commandir_data_buffer[6];
					commandir_tx_end[tx_order[i]*4+3] = commandir_data_buffer[5];
					
					commandir_last_signal_id[ tx_order[i] ] = commandir_data_buffer[9];
					
					recalc_tx_available(tx_order[i]); 
					pipeline_check();
					if(top_signalq > 0)
					{
						read_received++;	
					}
					
					// This ALSO implies there's NO MORE RX DATA.
					lirc_t lirc_zero_buffer[2] = {0, 0};
					
					int tmp4 = 0;
					if(zeroterminated>1001)
					{
						// Send LIRC a 0,0 packet to allow IMMEDIATE decoding
						if(insert_fast_zeros > 0)
						{
							tmp4 = write(child_pipe_write, lirc_zero_buffer, sizeof(lirc_t)*insert_fast_zeros);
						}
						zeroterminated = 0;
					}
					else
					{
						if((zeroterminated < 1000) && (zeroterminated > 0))
							zeroterminated += 1000;
						if(zeroterminated > 1000)
							zeroterminated++;
					}
					
					break;
				}
						
						
				if(commandir_data_buffer[0]==RX_HEADER_EVENTS)
				{
					for(j=1; j<(read_retval); j++)
					{
						raise_event(commandir_data_buffer[j]+tx_order[i]*0x10);
					}
				}
				else
				{
					if( (commandir_data_buffer[0]==RX_HEADER_DATA) && 
						(commandir_rx_num==tx_order[i]) )
					{
						if(rx_hold==0)	// Only if we should be listening for remote cmds
						{
							zeroterminated = 1;
							conv_retval = commandir2_convert_RX(
								(unsigned short *)&commandir_data_buffer[2], 
								commandir_data_buffer[1]);
							read_received = conv_retval; // header
						}
					}
				}
				break;
			
			case HW_COMMANDIR_MINI:
			
				max_read = 5;
				while(max_read--){
					
					read_retval = usb_bulk_read(
						open_commandir_devices[ tx_order[i] ].cmdir_udev,
						1,
						(char *)commandir_data_buffer,
						64,
						USB_TIMEOUT_MS);
							
					if (!(read_retval == MAX_HW_MINI_PACKET)) 
					{
						if(read_retval == -19){
							logprintf(LOG_ERR, "Read Error - CommandIR probably unplugged");
						}
						else
						{
							logprintf(LOG_ERR, 
								"Didn't receive a full packet from a Mini! - err %d ."
								, read_retval);
						}
						
						hardware_disconnect(tx_order[i]);
						hardware_scan();
						break; 
					}

					
					if ( (commandir_data_buffer[1] > 0)  && 
						(commandir_rx_num==tx_order[i]) ) 
					{
						conv_retval = cmdir_convert_RX(commandir_data_buffer);
						
						read_received += commandir_data_buffer[1];					
						
						if(commandir_data_buffer[1] < 20)
						{
							// Lots of hardware buffer room left; don't tie up CPU
							break;
						}
					}
					else
					{
						break;
					}
				} // while; should only repeat if there's more RX data
				
				/* CommandIR Mini only has 1 buffer  */
				commandir_tx_start[tx_order[i]*4] = 0;
				commandir_tx_start[tx_order[i]*4+1] = 0;
				commandir_tx_start[tx_order[i]*4+2] = 0;
				commandir_tx_start[tx_order[i]*4+3] = 0;
				
				commandir_tx_end[tx_order[i]*4] = commandir_data_buffer[2];
				commandir_tx_end[tx_order[i]*4+1] = commandir_data_buffer[2];
				commandir_tx_end[tx_order[i]*4+2] = commandir_data_buffer[2];
				commandir_tx_end[tx_order[i]*4+3] = commandir_data_buffer[2];
				
				/* .. and it can't pipeline... */
				commandir_last_signal_id[i] = lastSendSignalID[i];
				recalc_tx_available(tx_order[i]); 
				pipeline_check();
				break;
			case HW_COMMANDIR_UNKNOWN:
					break;
		} // end switch
	} // for each attached hardware device
	return read_received;
}

static void setEmitterMask(int bitmask)
{
	channels_en[0] = bitmask & 0x0F;
	channels_en[1] = (bitmask >> 4) & 0xfF;
	channels_en[2] = (bitmask >> 8) & 0xfF;
	channels_en[3] = (bitmask >> 12) & 0xfF;
}


static int commandir2_convert_RX(unsigned short *bufferrx, 
	unsigned char numvalues)
{
	// convert hardware timestamp values to elapsed time values
	
	int i;
	int curpos = 0;
	int bytes_w = 0;
	lirc_t lirc_data_buffer[256];	
	
	i=0;
	int pca_count = 0;
	int overflows = 0;
	
	while(curpos < numvalues )
	{
		pca_count = (bufferrx[curpos] & 0x3fff) << 2; 
		pca_count = pca_count / 12;
		if(bufferrx[curpos] & COMMANDIR_2_OVERFLOW_MASK)
		{
			overflows = bufferrx[curpos+1];
			lirc_data_buffer[i] =  pca_count + (overflows * 0xffff / 12);
			
			if(bufferrx[curpos] & COMMANDIR_2_PULSE_MASK)
			{
				lirc_data_buffer[i] |= PULSE_BIT;
			}
			curpos++;
		
		}
		else
		{
			lirc_data_buffer[i] = pca_count;
			if(bufferrx[curpos] & COMMANDIR_2_PULSE_MASK)
			{
				lirc_data_buffer[i] |= PULSE_BIT;
			}
		}
		
		curpos++;
		i++;
		if(i> 255)
		{
			break;
		}
	}
		
 	bytes_w = write(child_pipe_write, lirc_data_buffer, sizeof(lirc_t)*i);
	
	if (bytes_w < 0)
	{
		logprintf(LOG_ERR, "Can't write to LIRC pipe! %d", child_pipe_write);
		return 0;
	}	
	
	return bytes_w;
}




// Originally from lirc_cmdir.c
static int cmdir_convert_RX(unsigned char *orig_rxbuffer)
{
	unsigned int num_data_values = 0;
	unsigned int num_data_bytes = 0;
	unsigned int asint1 = 0, asint2 = 0, overflows = 0;
	int i;
	int bytes_w;	// Pipe write
	lirc_t lirc_data_buffer[256];	
	

	num_data_bytes = orig_rxbuffer[1];
	
	/* check if num_bytes is multiple of 3; if not, error  */
	if (num_data_bytes%3 > 0) return -1;
	if (num_data_bytes > 60) return -3; 
	if (num_data_bytes < 3) return -2;
	
	num_data_values = num_data_bytes/3;
	
	asint2 = orig_rxbuffer[3] + orig_rxbuffer[4] * 0xff;
	if(last_mc_time==-1)
	{
		// The first time we run there's no previous time value
		last_mc_time = asint2 - 110000;	
		if(last_mc_time < 0) last_mc_time+=0xffff;
	}
	
	asint1 = last_mc_time;
	overflows = orig_rxbuffer[5];
	
	for(i=2; i<num_data_values+2; i++)
	{
		if(overflows < 0xff)
		{
			// space
			lirc_data_buffer[i-2] = get_time_value(asint1,
				 asint2, overflows) - 26;
		} 
		else 
		{	// pulse
			lirc_data_buffer[i-2] = get_time_value(asint1,
				 asint2, 0) + 26;
			lirc_data_buffer[i-2] |= PULSE_BIT;
		}	
		asint1 = asint2; 
		asint2 = orig_rxbuffer[i*3] + orig_rxbuffer[i*3+1] * 0xff;
		overflows = orig_rxbuffer[i*3+2];	
	}
	last_mc_time = asint1;
	
	
 	bytes_w = write(child_pipe_write, lirc_data_buffer, sizeof(lirc_t)*num_data_values);
	
	if (bytes_w < 0)
	{
		logprintf(LOG_ERR, "Can't write to LIRC pipe! %d", child_pipe_write);
		goto done;
	}	
	
done:
	return bytes_w;

}




static unsigned int get_time_value(unsigned int firstint, 
	unsigned int secondint, unsigned char overflow) 
{	
	/* get difference between two MCU timestamps, CommandIR Mini version  */
	unsigned int t_answer = 0;
	
	if (secondint > firstint) 
	{
		t_answer = secondint - firstint + overflow*0xffff;
	} 
	else 
	{
		if (overflow > 0) 
		{
			t_answer = (65536 - firstint) + secondint + (overflow - 1)*0xffff - 250;
		} 
		else 
		{
			t_answer = (65536 - firstint) + secondint;
		}
	}

	/* clamp to long signal  */
	if (t_answer > 16000000) t_answer = PULSE_MASK;
	return t_answer;
}


static void raise_event(unsigned int eventid)
{
	/* Raise an LIRC Event by
	 * Generating lirc_t Pattern
	 */
	static lirc_t event_data[18] = {LIRCCODE_GAP, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
	int i, bytes_w;

	// 	logprintf(LOG_ERR, "Raising event %d", eventid);
	for(i=0; i<8; i++)
	{
		if( (eventid & 0x80) )
		{
			event_data[i*2+1] = signal_base[0][0];
			event_data[i*2+2] = signal_base[0][1];
		}
		else
		{
			event_data[i*2+1] = signal_base[1][0];
			event_data[i*2+2] = signal_base[1][1];
		}
		eventid = eventid << 1;
	}
	
 	event_data[16] = LIRCCODE_GAP*4;
 	
 	bytes_w = write(child_pipe_write, event_data, sizeof(lirc_t) * 17);
	
	if (bytes_w < 0)
	{
		logprintf(LOG_ERR, "Can't write to LIRC pipe! %d", child_pipe_write);
	}	
	
}

static int commandir_decode(char *command)
{
	// Decode the signal to a number, just like LIRC; 
	// there's probably a built-in way to do this.
	int i;
	int code = 0;
	
	lirc_t *codes;	
	codes = (lirc_t *)command;
	
	for(i=0; i<15; i+=2)
	{
		code = code << 1;
		if(codes[i]==100)
			code |= 1;
	}
	return code;
}

