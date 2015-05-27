/******************************************************************************
 * Copyright 2013-2014 Espressif Systems (Wuxi)
 *
 * FileName: user_main.c
 *
 * Description: entry file of user application
 *
 * Modification history:
 *     2014/12/1, v1.0 create this file.
*******************************************************************************/
#include "esp_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "mad.h"

/*
Mem usage:
layer3: 744 bytes rodata
*/


//#define server_ip "192.168.40.115"
#define server_ip "192.168.1.4"
//#define server_ip "192.168.4.100"
#define server_port 1234

#define READER_THREAD
//#define UART_AUDIO
//#define FROMFLASH
#define SPI_BUFFER

#define READERBUFSIZE 5100
#define READCHUNKSIZE 256

#ifdef FROMFLASH
char ICACHE_RODATA_ATTR mp3data[]={
#include "mp3.dat"
};
#endif

struct madPrivateData {
	int fd;
	xSemaphoreHandle muxBufferBusy;
	xSemaphoreHandle semNeedRead;
#ifdef READER_THREAD
	char buff[READERBUFSIZE];
	int buffPos;
#endif
#ifdef FROMFLASH
	char *fpos;
#endif
};

#ifdef UART_AUDIO
#define printf(a, ...) while(0)
#endif

#ifdef SPI_BUFFER

#define SPI 			0
#define HSPI			1


void spiRamInit() {
	 //hspi overlap to spi, two spi masters on cspi
	//#define HOST_INF_SEL 0x3ff00028 
	SET_PERI_REG_MASK(0x3ff00028, BIT(7));
	//SET_PERI_REG_MASK(HOST_INF_SEL, PERI_IO_CSPI_OVERLAP);

	//set higher priority for spi than hspi
	SET_PERI_REG_MASK(SPI_EXT3(SPI), 0x1);
	SET_PERI_REG_MASK(SPI_EXT3(HSPI), 0x3);
	SET_PERI_REG_MASK(SPI_USER(HSPI), BIT(5));

	//select HSPI CS2 ,disable HSPI CS0 and CS1
	CLEAR_PERI_REG_MASK(SPI_PIN(HSPI), SPI_CS2_DIS);
	SET_PERI_REG_MASK(SPI_PIN(HSPI), SPI_CS0_DIS |SPI_CS1_DIS);

	//SET IO MUX FOR GPIO0 , SELECT PIN FUNC AS SPI CS2
	//IT WORK AS HSPI CS2 AFTER OVERLAP(THERE IS NO PIN OUT FOR NATIVE HSPI CS1/2)
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_SPICS2);

	WRITE_PERI_REG(SPI_CLOCK(HSPI), 
					(((7)&SPI_CLKDIV_PRE)<<SPI_CLKDIV_PRE_S)|
					(((4)&SPI_CLKCNT_N)<<SPI_CLKCNT_N_S)|
					(((4)&SPI_CLKCNT_H)<<SPI_CLKCNT_H_S)|
					(((2)&SPI_CLKCNT_L)<<SPI_CLKCNT_L_S));
}

#define SPI_W(i, j)                   (REG_SPI_BASE(i) + 0x40 + ((j)*4))


//n=1..64
void spiRamRead(int addr, char *buff, int len) {
	int i;
	int *p=(int*)buff;
	SET_PERI_REG_MASK(SPI_USER(HSPI), SPI_CS_SETUP|SPI_CS_HOLD|SPI_USR_COMMAND|SPI_USR_ADDR|SPI_USR_MISO);
	CLEAR_PERI_REG_MASK(SPI_USER(HSPI), SPI_FLASH_MODE|SPI_USR_MOSI);
	WRITE_PERI_REG(SPI_USER1(HSPI), ((0&SPI_USR_MOSI_BITLEN)<<SPI_USR_MOSI_BITLEN_S)| //no data out
			((((8*len)-1)&SPI_USR_MISO_BITLEN)<<SPI_USR_MISO_BITLEN_S)| //len bits of data in
			((23&SPI_USR_ADDR_BITLEN)<<SPI_USR_ADDR_BITLEN_S)); //address is 24 bits A0-A23
	WRITE_PERI_REG(SPI_USER2(HSPI), (((7&SPI_USR_COMMAND_BITLEN)<<SPI_USR_COMMAND_BITLEN_S) | 0x03));
	SET_PERI_REG_MASK(SPI_CMD(HSPI), SPI_USR);
	while(READ_PERI_REG(SPI_CMD(HSPI))&SPI_USR) ;
	for (i=0; i<(len+3)/4; i++) {
		p[i]=READ_PERI_REG(SPI_W(HSPI, i));
	}
}

//n=1..64
void spiRamWrite(int addr, char *buff, int len) {
	int i;
	int *p=(int*)buff;
	SET_PERI_REG_MASK(SPI_USER(HSPI), SPI_CS_SETUP|SPI_CS_HOLD|SPI_USR_COMMAND|SPI_USR_ADDR|SPI_USR_MOSI);
	CLEAR_PERI_REG_MASK(SPI_USER(HSPI), SPI_FLASH_MODE|SPI_USR_MISO);
	WRITE_PERI_REG(SPI_USER1(HSPI), ((((8*len)-1)&SPI_USR_MOSI_BITLEN)<<SPI_USR_MOSI_BITLEN_S)| //len bitsbits of data out
			((0&SPI_USR_MISO_BITLEN)<<SPI_USR_MISO_BITLEN_S)| //no data in
			((23&SPI_USR_ADDR_BITLEN)<<SPI_USR_ADDR_BITLEN_S)); //address is 24 bits A0-A23
	WRITE_PERI_REG(SPI_ADDR(HSPI), addr<<8); //write address
	WRITE_PERI_REG(SPI_USER2(HSPI), (((7&SPI_USR_COMMAND_BITLEN)<<SPI_USR_COMMAND_BITLEN_S) | 0x02));
	for (i=0; i<(len+3)/4; i++) {
		WRITE_PERI_REG(SPI_W(HSPI, (i)), p[i]);
	}
	SET_PERI_REG_MASK(SPI_CMD(HSPI), SPI_USR);
	while(READ_PERI_REG(SPI_CMD(HSPI))&SPI_USR) ;
}

#endif


struct madPrivateData madParms;

void render_sample_block(short *short_sample_buff, int no_samples) {
	int i, s;
	static int err=0;
	char samp[]={0x00, 0x01, 0x11, 0x15, 0x55, 0x75, 0x77, 0xf7, 0xff};
#ifdef UART_AUDIO
	for (i=0; i<no_samples; i++) {
		s=short_sample_buff[i];
		s+=err;
		if (s>32867) s=32767;
		if (s<-32768) s=-32768;
		uart_tx_one_char(0, samp[(s >> 14)+4]);
		err=s-((s>>14)<<14);
	}
#endif
//	printf("rsb %04x %04x\n", short_sample_buff[0], short_sample_buff[1]);
}

void set_dac_sample_rate(int rate) {
	printf("sr %d\n", rate);
}

//The mp3 read buffer. 2106 bytes should be enough for up to 48KHz mp3s according to the sox sources. Used by libmad.
#define READBUFSZ 2106
static char readBuf[READBUFSZ]; 

void ICACHE_FLASH_ATTR memcpyAligned(char *dst, char *src, int len) {
	int x;
	int w, b;
	for (x=0; x<len; x++) {
		b=((int)src&3);
		w=*((int *)(src-b));
		if (b==0) *dst=(w>>0);
		if (b==1) *dst=(w>>8);
		if (b==2) *dst=(w>>16);
		if (b==3) *dst=(w>>24);
		dst++; src++;
	}
}


static enum  mad_flow ICACHE_FLASH_ATTR input(void *data, struct mad_stream *stream) {
	int n, i;
	int rem, canRead;
	struct madPrivateData *p = (struct madPrivateData*)data;
	//Shift remaining contents of buf to the front
	rem=stream->bufend-stream->next_frame;
	memmove(readBuf, stream->next_frame, rem);

#ifdef READER_THREAD
	//Wait until there is enough data in the buffer. This only happens when the data feed rate is too low, and shouldn't normally be needed!
	do {
		xSemaphoreTake(p->muxBufferBusy, portMAX_DELAY);
		canRead=((p->buffPos)>=(sizeof(readBuf)-rem));
		xSemaphoreGive(p->muxBufferBusy);
		if (!canRead) {
			printf("Buf uflow %d < %d \n", (p->buffPos), (sizeof(readBuf)-rem));
			vTaskDelay(100/portTICK_RATE_MS);
		}
	} while (!canRead);


	//Read in bytes from buffer
	xSemaphoreTake(p->muxBufferBusy, portMAX_DELAY);
	n=sizeof(readBuf)-rem; //amount of bytes to re-fill the buffer with
	memcpy(&readBuf[rem], p->buff, n);
	//Shift remaining contents of readBuff to the front
	memmove(p->buff, &p->buff[n], sizeof(p->buff)-n);
	p->buffPos-=n;
	xSemaphoreGive(p->muxBufferBusy);

	//Let reader thread read more data.
	xSemaphoreGive(p->semNeedRead);
#elif defined(FROMFLASH)
	//Copy from flash
	n=sizeof(readBuf)-i;
	printf("Copy %p, %d bytes\n", p->fpos, n);
	memcpyAligned(&readBuf[i], p->fpos, n);
	printf("Flashbuf: %02x %02x %02x %02x %02x\n", readBuf[i], readBuf[i+1], readBuf[i+2], readBuf[i+3], readBuf[i+4]);
	p->fpos+=(sizeof(readBuf)-i);
#else
	i=rem;
	while (i<sizeof(readBuf)) {
		n=read(p->fd, &readBuf[i], sizeof(readBuf)-i);
		i+=n;
	}
#endif
	mad_stream_buffer(stream, readBuf, sizeof(readBuf));
	return MAD_FLOW_CONTINUE;
}

//Unused, the NXP implementation uses render_sample_block
static enum mad_flow ICACHE_FLASH_ATTR output(void *data, struct mad_header const *header, struct mad_pcm *pcm) {
	 return MAD_FLOW_CONTINUE;
}

static enum mad_flow ICACHE_FLASH_ATTR error(void *data, struct mad_stream *stream, struct mad_frame *frame) {
//	struct madPrivateData *p = (struct madPrivateData*)data;
	printf("dec err 0x%04x (%s)\n", stream->error, mad_stream_errorstr(stream));
	return MAD_FLOW_CONTINUE;
}


int openConn() {
	while(1) {
		int n, i;
		struct sockaddr_in remote_ip;
		int sock=socket(PF_INET, SOCK_STREAM, 0);
		if (sock==-1) {
//			printf("Client socket create error\n");
			continue;
		}
		n=1;
		setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &n, sizeof(n));
		n=4096;
		setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &n, sizeof(n));
		bzero(&remote_ip, sizeof(struct sockaddr_in));
		remote_ip.sin_family = AF_INET;
		remote_ip.sin_addr.s_addr = inet_addr(server_ip);
		remote_ip.sin_port = htons(server_port);
//		printf("Connecting to client...\n");
		if (connect(sock, (struct sockaddr *)(&remote_ip), sizeof(struct sockaddr))!=00) {
			close(sock);
			printf("Conn err.\n");
			vTaskDelay(1000/portTICK_RATE_MS);
			continue;
		}
		return sock;
	}
}


void ICACHE_FLASH_ATTR tskmad(void *pvParameters){
	struct mad_decoder decoder;
	struct madPrivateData *p=&madParms; //pvParameters;
#if !defined(READER_THREAD) && !defined(FROMFLASH)
	p->fd=openConn();
#endif
//	printf("MAD: Decoder init.\n");
	mad_decoder_init(&decoder, p, input, 0, 0 , output, error, 0);
	mad_decoder_run(&decoder, MAD_DECODER_MODE_SYNC);
	mad_decoder_finish(&decoder);
//	printf("MAD: Decode done.\n");
}

#ifdef READER_THREAD
void ICACHE_FLASH_ATTR tskreader(void *pvParameters) {
	struct madPrivateData *p=&madParms;//pvParameters;
	fd_set fdsRead;
	int n;
	p->fd=-1;
	while(1) {
		if (p->fd==-1) p->fd=openConn();
		printf("Resuming read, can read %d bytes...\n", sizeof(p->buff)-p->buffPos);
		while (p->buffPos!=sizeof(p->buff)) {
			//Wait for more data
			FD_ZERO(&fdsRead);
			FD_SET(p->fd, &fdsRead);
			lwip_select(p->fd+1, &fdsRead, NULL, NULL, NULL);
			//Take buffer mux and read data.
			xSemaphoreTake(p->muxBufferBusy, portMAX_DELAY);

			n=READCHUNKSIZE;
			if (n>sizeof(p->buff)-p->buffPos) n=sizeof(p->buff)-p->buffPos;
			n=read(p->fd, &p->buff[p->buffPos], n);
			if (n<=0) {
				close(p->fd);
				p->fd=-1;
				break;
			}
			p->buffPos+=n;
			xSemaphoreGive(p->muxBufferBusy);
			printf("Sock: %d bytes.\n", n);
			if (n==0) break; //ToDo: handle eof better
		}
		printf("Read done.\n");
		//Try to take the semaphore. This will wait until the mad task signals it needs more data.
		xSemaphoreTake(p->semNeedRead, portMAX_DELAY);
	}
}
#endif

void ICACHE_FLASH_ATTR tskconnect(void *pvParameters) {
	vTaskDelay(3000/portTICK_RATE_MS);
//	printf("Connecting to AP...\n");
	wifi_station_disconnect();
//	wifi_set_opmode(STATIONAP_MODE);
	if (wifi_get_opmode() != STATION_MODE) { 
		wifi_set_opmode(STATION_MODE);
	}
//	wifi_set_opmode(SOFTAP_MODE);

	struct station_config *config=malloc(sizeof(struct station_config));
	memset(config, 0x00, sizeof(struct station_config));
//	sprintf(config->ssid, "wifi-2");
//	sprintf(config->password, "thesumof6+6=12");
	sprintf(config->ssid, "Sprite");
	sprintf(config->password, "pannenkoek");
	wifi_station_set_config(config);
	wifi_station_connect();
	free(config);
//	printf("Connection thread done.\n");

	//tskMad seems to need at least 2450 bytes of RAM.
	if (xTaskCreate(tskmad, "tskmad", 2450, NULL, 12, NULL)!=pdPASS) printf("ERROR! Couldn't create MAD task!\n");
#ifdef READER_THREAD
	if (xTaskCreate(tskreader, "tskreader", 230, NULL, 11, NULL)!=pdPASS) printf("ERROR! Couldn't create reader task!\n");
#endif
	vTaskDelete(NULL);
}

void ICACHE_FLASH_ATTR tskspitst(void *pvParameters) {
	int a[4];
#ifdef SPI_BUFFER
	spiRamInit();
	printf("SPI ram INITED\n");
	while(1) {
		a[0]=0xdeadbeef;
		a[1]=0xcafebabe;
		a[2]=0x12345678;
		a[3]=0xaa55aa55;
		printf("Before: %x %x %x %x\n", a[0], a[1], a[2], a[3]);
//		spiRamWrite(0x10, (char*)a, 8);
//		spiRamWrite(0x18, (char*)&a[2], 8);
		spiRamWrite(0, (char*)a, 16);
		spiRamRead(0, (char*)a, 16);
		printf("After: %x %x %x %x\n", a[0], a[1], a[2], a[3]);
		vTaskDelay(500/portTICK_RATE_MS);
	}
#endif


}


void ICACHE_FLASH_ATTR
user_init(void)
{
#ifdef UART_AUDIO
	UART_SetBaudrate(0, 481000);
#else
	UART_SetBaudrate(0, 115200);
#endif

#ifdef FROMFLASH
	madParms.fpos=mp3data;
#endif
	madParms.muxBufferBusy=xSemaphoreCreateMutex();
	vSemaphoreCreateBinary(madParms.semNeedRead);
//	printf("Starting tasks...\n");
	xTaskCreate(tskconnect, "tskconnect", 200, NULL, 3, NULL);


	if (xTaskCreate(tskspitst, "tskspitst", 230, NULL, 11, NULL)!=pdPASS) printf("ERROR! Couldn't create reader task!\n");

}

