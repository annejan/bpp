#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "hexdump.h"


#include "structs.h"
#include "chksign.h"
#include "defec.h"
#include "serdec.h"
#include "hldemux.h"

#include "subtitle.h"
#include "blockdecode.h"
#include "bd_emu.h"
#include "bd_flatflash.h"
#include "hkpackets.h"

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs.h"
#include "powerdown.h"

#include "freertos/ringbuf.h"

esp_err_t event_handler(void *ctx, system_event_t *event)
{
    return ESP_OK;
}


typedef struct {
	uint8_t mac[6];
} __attribute__((packed)) MacAddr;

typedef struct {
	int16_t fctl;
	int16_t duration;
	MacAddr addr1;
	MacAddr addr2;
	MacAddr addr3;
	int16_t seqctl;
	MacAddr addr4;
	unsigned char payload[];
} __attribute__((packed)) WifiHdr;

typedef struct {
	uint8_t dsap;
	uint8_t ssap;
	uint8_t control1;
	uint8_t control2;
	unsigned char payload[];
} __attribute__((packed)) LlcHdr;

typedef struct {
	uint8_t oui[3];
	uint16_t proto;
	unsigned char payload[];
} __attribute__((packed)) LlcHdrSnap;

typedef struct {
	MacAddr src;
	MacAddr dst;
	uint16_t len;
	unsigned char payload[];
} __attribute__((packed)) EthHdr;

typedef struct {
	uint8_t verihl;
	uint8_t tos;
	uint16_t len;
	uint16_t id;
	uint16_t flag;
	uint8_t ttl;
	uint8_t proto;
	uint16_t hdrcsum;
	uint32_t src;
	uint32_t dst;
	unsigned char payload[];
} __attribute__((packed)) IpHdr;


typedef struct {
	uint16_t srcPort;
	uint16_t dstPort;
	uint16_t len;
	uint16_t chs;
	unsigned char payload[];
} __attribute__((packed)) UdpHdr;


void printmac(MacAddr *mac) {
	int x;
	for (x=0; x<6; x++) printf("%02X%s", mac->mac[x], x!=5?":":"");
	printf(" ");
}

void printip(uint32_t ip) {
	uint8_t *p=(uint8_t*)&ip;
	printf("%d.%d.%d.%d", p[0], p[1], p[2], p[3]);
}

RingbufHandle_t *packetRingbuf;

void sniffcb(void *buf, wifi_promiscuous_pkt_type_t type) {
//	printf("Sniffed: type %d\n", type);
	if (type==WIFI_PKT_DATA) {
		//The buffer consists of many layers of headers. Peel this onion until we've
		//arrived at the juicy UDP inner load.
		wifi_promiscuous_pkt_t *p=(wifi_promiscuous_pkt_t*)buf;
		int len=p->rx_ctrl.sig_len;
		WifiHdr *wh=(WifiHdr*)p->payload;
		len-=sizeof(WifiHdr);
		if (len<0) return;
		int fctl=ntohs(wh->fctl);
		if (fctl&0x0040) return; //Encrypted, can't handle this.
		if ((fctl&0xF00)!=0x800) return; //we only want data packets
		LlcHdr *lch=(LlcHdr*)wh->payload;
		uint8_t *pl=(uint8_t*)lch->payload;
		if ((lch->control1&3)==3) {
			pl--; //only has 8-bit control header; payload starts earlier
			len-=sizeof(LlcHdr)-1;
		} else {
			len-=sizeof(LlcHdr);
		}
		if (len<0) return;
		IpHdr *iph;
		if (lch->dsap==0xAA) {
			//Also has SNAP data
			LlcHdrSnap *lchs=(LlcHdrSnap*)pl;
			len-=sizeof(LlcHdrSnap);
			if (len<0) return;
			iph=(IpHdr*)lchs->payload;
		} else {
			iph=(IpHdr*)pl;
		}

		len-=sizeof(IpHdr);
		if (len<0) return;

		if ((iph->verihl>>4)!=4) return; //discard non-ipv4 packets
		int ip_ihl=(iph->verihl&0xf);
		if (ip_ihl<5) return; //invalid

		len-=(ip_ihl-5)*4;
		if (len<0) return;

//		hexdump(&iph->payload[(ip_ihl-5)*4], len-0x20);
		if (len<sizeof(UdpHdr)) return;
		UdpHdr *uh=(UdpHdr*)&iph->payload[(ip_ihl-5)*4];
		if (ntohs(uh->len) < len-4) return; //-4 because WiFi packets have 4-byte CRC appended
		int udppllen=ntohs(uh->len)-sizeof(UdpHdr);
//		printf("Rem len %d udp len %d ", len-4, ntohs(uh->len));
//		printf("Packet ");
//		printip(iph->src);
//		printf(":%d -> ", ntohs(uh->srcPort));
//		printip(iph->dst);
//		printf(":%d\n", ntohs(uh->dstPort));

		if (ntohs(uh->dstPort)!=2017) return;
		
		xRingbufferSend(packetRingbuf, uh->payload, udppllen, 0);
	}
}


BlockDecodeHandle *otablockdecoder;


void parseTask(void *arg) {
	int t=0;
	printf("ParseTask started.");
	while(1) {
		size_t len;
		uint8_t *p=xRingbufferReceive(packetRingbuf, &len, portMAX_DELAY);
		chksignRecv(p, len);
		vRingbufferReturnItem(packetRingbuf, p);
//		printf("Parsed.\n");
		t++;
		if ((t&63)==0) blockdecodeStatus(otablockdecoder);
	}
}



void flashDone(uint32_t changeId, void *arg) {
	esp_partition_t *otapart=(esp_partition_t*)arg;
	printf("Flash done!\n");
	esp_err_t r=esp_ota_set_boot_partition(otapart);
	if (r!=ESP_OK) {
		printf("Huh? esp_ota_set_boot_partition failed.\n");
	} else {
		blockdecodeShutDown(otablockdecoder);
		esp_restart();
	}
}

void doDeepSleep(int delayMs, void *arg) {
	delayMs-=3000; //to compensate for startup delay
	if (delayMs<5000) return; //not worth sleeping
	printf("Sleeping for %d ms...\n", delayMs);
	blockdecodeShutDown(otablockdecoder);
	esp_deep_sleep_enable_timer_wakeup(delayMs*1000);
	esp_deep_sleep_start();
}


void app_main(void)
{
    nvs_flash_init();
    tcpip_adapter_init();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
//    ESP_ERROR_CHECK( esp_wifi_start() );

	powerDownMgrInit(doDeepSleep, NULL);

	printf("********************\n");
	printf("* V 9              *\n");
	printf("********************\n");


	chksignInit(defecRecv);
	defecInit(serdecRecv, 1400);
	serdecInit(hldemuxRecv);
	
	//Grab last OTA firmware change ID so we don't redundantly update the OTA region
	nvs_handle nvsh=NULL;
	nvs_open("bpp", NVS_READWRITE, &nvsh);
	uint32_t chgid=0;
	nvs_get_u32(nvsh, "lastotaid", &chgid);
	nvs_close(nvsh);

	//Figure out which OTA partition we're *not* running from, pass that to the blockdev.
	const esp_partition_t *otapart=esp_ota_get_next_update_partition(NULL);
	

	BlockdefIfFlatFlashDesc bdesc={
		.major=otapart->type,
		.minor=otapart->subtype,
		.doneCb=flashDone,
		.doneCbArg=(void*)otapart,
		.minChangeId=chgid
	};
	int otasize=otapart->size-4096;
	otablockdecoder=blockdecodeInit(1, otasize, &blockdefIfFlatFlash, &bdesc);
	printf("Initialized ota blockdev listener; maj=%x min=%x size=%d\n", bdesc.major, bdesc.minor, otasize);

	subtitleInit();
	hkpacketsInit();

	packetRingbuf=xRingbufferCreate(32*1024, RINGBUF_TYPE_NOSPLIT);

	printf("Starting promisc mode\n");
	ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(sniffcb));
	ESP_ERROR_CHECK(esp_wifi_set_promiscuous(1));
	ESP_ERROR_CHECK(esp_wifi_set_channel(11,WIFI_SECOND_CHAN_NONE));
	
	xTaskCreatePinnedToCore(parseTask, "parse", 8192, NULL, 3, NULL, 1);
}

