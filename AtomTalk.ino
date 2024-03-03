// オーディオデータをUDP受信してスピーカー再生
// 本機：192.169.0.212:60000 固定
// テストプログラム「udpsnd2」(VC、udprcv2で受信した音声データを送信)
// オーディオデータは、16ビット、16000Hz、モノラル、LSBファースト（Intel）
// UDPパケットデータサイズは、800バイト固定
// 
// 1秒間隔で生存確認受信（0x55、0xAA）、返信（0xA5、0x5A）
// 
// ボードマネージャー
// M5Stack by M5Stack
// ライブラリ
// M5Stack by M5Stack
// M5Atom by M5Stack


#include <driver/i2s.h>
#include <M5Atom.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#define BUFSIZE 1024

#define CONFIG_I2S_BCK_PIN      19
#define CONFIG_I2S_LRCK_PIN     33
#define CONFIG_I2S_DATA_PIN     22
#define CONFIG_I2S_DATA_IN_PIN  23

#define SPEAKER_I2S_NUMBER      I2S_NUM_0

#define MODE_MIC                0
#define MODE_SPK                1

#define BUFFER_LEN  800
bool REC;

#define BUFFNUM       30   // 音声格納バッファ
#define WAITPACKET    10   // 再生開始蓄積パケット数

#define DATASIZE     800   // 32000B/sec -> 800B = 25msec分
#define STORAGE_LEN 8000   // 10packets
int wpos;
int rpos;
int start;
int tskstop;

const char* ssid     = "xxxxxx";  // WiFi SSID
const char* password = "xxxxxx";  // WiFi パスワード
const int   port     = 60000;

// IP固定する
IPAddress ip(192, 168, 0, 212);
IPAddress gateway(192, 168, 0, 1);
IPAddress subnet(255, 255, 255, 0);

WiFiUDP udp;
int signalok = 0;
int wifiok = 0;
unsigned int rcvnum = 0;
unsigned int playnum = 0;
unsigned char playbuff[BUFFNUM][DATASIZE];

int total = 0;
int count = 0;

// iPhoneからのシグナルを元にiPhoneのIPアドレスと送信ポートを格納する
IPAddress sip = {192, 168, 0, 212};
int     sport = 60000;

void(* resetboard) (void) = 0; // declare reset function @ address 0

// WiFi接続
void setup_wifi()
{
	int status = WL_IDLE_STATUS;
	
	WiFi.mode(WIFI_STA);
	WiFi.config(ip, gateway, subnet);  // static_ip
	WiFi.begin(ssid, password);
	delay(100);
	
	status = WiFi.status();
	int cnt = 0;
	while(status != WL_CONNECTED){
		cnt++;
		if((cnt % 100) == 0){  // 10sec経って接続できなければ再発行
			Serial.println("\nWiFi : try re-connect!");
			WiFi.begin(ssid, password);
			delay(100);
		}
		if(cnt > 600){  // 60sec経っても接続できなければリセット
			// WiFi接続失敗 -> リブート
			Serial.println("\nERROR : cannot connect! & reset!");
			resetboard();  // call reset
		}
		status = WiFi.status();
		delay(100);
	}
	Serial.println("\nConnected");
	Serial.println(WiFi.localIP());
	delay(100);
	
	udp.begin(port);
	wifiok = 1;
	delay(100);
}


// 音声送信用タスク
void i2sSendTask(void* arg)
{
	unsigned char vbuff[BUFFER_LEN];
	
	Serial.println("SEND START !");
	
	Serial.printf("S IP = %d.%d.%d.%d\n", sip[0], sip[1], sip[2], sip[3]);
	Serial.printf("S PORT = %d\n", sport);
	
	vTaskDelay(100);
	
	// 録音処理
	while(REC){
		size_t transBytes;
		
		// I2Sからデータ取得
		i2s_read(I2S_NUM_0, (char*)vbuff, BUFFER_LEN, &transBytes, (100 / portTICK_RATE_MS));
		
		// UDP 送信
		udp.beginPacket(sip, sport);
		udp.write(vbuff, transBytes);
		udp.endPacket();
		
		//Serial.printf("SND %d\n", count++);
		vTaskDelay(1 / portTICK_RATE_MS);
	}
	
	Serial.println("SEND STOP !");
	
	vTaskDelay(100);
	
	// iPhone側再生停止用 UDP送信
	vbuff[0] = 0x00;
	vbuff[1] = 0x00;
	udp.beginPacket(sip, sport);
	udp.write(vbuff, 2);
	udp.endPacket();
	
	// タスク削除
	vTaskDelete(NULL);
}




void InitI2SSpeakerOrMic(int mode)
{
	esp_err_t err = ESP_OK;
	
	i2s_driver_uninstall(SPEAKER_I2S_NUMBER);
	i2s_config_t i2s_config = {
		.mode                 = (i2s_mode_t)(I2S_MODE_MASTER),
		.sample_rate          = 16000,
		.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
		.channel_format       = I2S_CHANNEL_FMT_ALL_RIGHT,
		.communication_format = I2S_COMM_FORMAT_I2S,
		.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
		.dma_buf_count        = 6,
		.dma_buf_len          = 60,
		.use_apll             = false,
		.tx_desc_auto_clear   = true,
		.fixed_mclk           = 0
	};
	
	if(mode == MODE_MIC){
		i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
		i2s_config.sample_rate = 8000; // なぜかPDM側が2倍速で取り込まれるので減速
	}
	else{
		i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
		i2s_config.sample_rate = 16000;
	}
	
	err += i2s_driver_install(SPEAKER_I2S_NUMBER, &i2s_config, 0, NULL);
	
	i2s_pin_config_t tx_pin_config = {
		.bck_io_num           = CONFIG_I2S_BCK_PIN,
		.ws_io_num            = CONFIG_I2S_LRCK_PIN,
		.data_out_num         = CONFIG_I2S_DATA_PIN,
		.data_in_num          = CONFIG_I2S_DATA_IN_PIN,
	};
	
	err += i2s_set_pin(SPEAKER_I2S_NUMBER, &tx_pin_config);
	
	if(mode != MODE_MIC){
		err += i2s_set_clk(SPEAKER_I2S_NUMBER, 16000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
	}
	
	i2s_zero_dma_buffer(SPEAKER_I2S_NUMBER);
}


// 再生用タスク
void i2sPlayTask(void* arg) 
{
	size_t bytes_written;
	bool loop = true;
	
	vTaskDelay(10);
	
	while(loop){
		if(start == 2){
			if(rcvnum >= playnum + DATASIZE){
				// 再生
				// Write Speaker
				i2s_write(SPEAKER_I2S_NUMBER, playbuff[rpos], DATASIZE, &bytes_written, portMAX_DELAY);
				
				if(rpos < (BUFFNUM-1)) rpos++;
				else                   rpos = 0;
				
				playnum += DATASIZE;
			}
		}
		
		if(tskstop != 0){
			// 終了
			loop = false;
			break;
		}
		vTaskDelay(1);
	}
	
	vTaskDelay(10);
	// タスク削除
	vTaskDelete(NULL);
}


// 初期化
void setup() 
{
	Serial.println("SETUP START...");
	
	M5.begin(true, false, true);
	delay(50);
	M5.dis.drawpix(0, CRGB(0, 0, 0));  // Black
	
	wpos = 0;
	rpos = 0;
	start = 0;
	rcvnum = 0;
	playnum = 0;
	tskstop = 0;
	
	total = 0;
	count = 0;
	
	REC = false;
	
	InitI2SSpeakerOrMic(MODE_MIC);
	delay(300);
	
	setup_wifi();
	
	Serial.println("SETUP COMPLETE !");
}


// 常時繰り返し
void loop() 
{
	unsigned char packetBuffer[BUFSIZE];
	
	M5.update();
	
	// シグナル無しのタイムアウト（300×1msec）
	if(signalok > 0 && REC == false){
		signalok--;
	}
	
	// LED設定
	if(wifiok == 0){
		M5.dis.drawpix(0, CRGB(0, 0, 0));  // Black
	}
	else{
		if(REC == true){
			// 音声送信中
			M5.dis.drawpix(0, CRGB(0, 255, 255));  // Cyan
		}
		else if(start != 0){
			// 音声受信中
			M5.dis.drawpix(0, CRGB(255, 0, 0));  // Red
		}
		else{
			if(signalok != 0){
				// 通信可能（iPhoneあり）
				M5.dis.drawpix(0, CRGB(0, 255, 0));  // Green
			}
			else{
				// WiFi接続OK
				M5.dis.drawpix(0, CRGB(0, 0, 255));  // Blue
			}
		}
	}
	
	// ボタン押下で音声送信処理
	if(M5.Btn.wasPressed()){
		Serial.println("BUTTON PRESS");
		// 送信開始 
		if(signalok != 0){
			REC = true;
			delay(100);
			// 音声送信タスク開始
			xTaskCreatePinnedToCore(i2sSendTask, "i2sSendTask", 4096, NULL, 1, NULL, 1);
		}
	}
	if(M5.Btn.wasReleased()){
		// 送信終了
		if(signalok != 0){
			REC = false;
			delay(100);
			// 音声送信後にUDP受信ができなくなる問題の為、ここでUDP再起動する
			udp.stop();
			delay(100);
			udp.begin(port);
			signalok = 600;  // シグナルタイムアウトリセット（3sec）
		}
		Serial.println("BUTTON RELEASE\n");
	}
	
	// 音声受信処理
	int packetSize = udp.parsePacket();
	
	if(REC == false){  // 音声送信中でない
		if(packetSize > DATASIZE){
			Serial.println("!!! PACKET SIZE ERROR !!!");
			delay(300);
		}
		
		if(packetSize > 0){
			udp.read(packetBuffer, packetSize);
			packetBuffer[packetSize] = 0;
			
			//Serial.printf("RCV[%dB]\n", packetSize);
			
			if(packetSize > 2){
				total += packetSize;
				count++;
			}
			else if(packetSize == 2){
				// シグナル受信したらiPhoneのIPアドレスと送信元ポートを取得
				sip = udp.remoteIP();
				sport = udp.remotePort();
				if(packetBuffer[0] == 0x55  && packetBuffer[1] == 0xAA){
					// UDP生存確認返信
					packetBuffer[0] = 0xA5;
					packetBuffer[1] = 0x5A;
					udp.beginPacket(sip, sport);
					udp.write(packetBuffer, 2);
					udp.endPacket();
					signalok = 600;  // シグナルタイムアウトリセット（3sec）
				}
			}
			
			if(packetSize == DATASIZE){
				memcpy(playbuff[wpos], packetBuffer, DATASIZE);
				if(wpos < (BUFFNUM-1)) wpos++;
				else                   wpos = 0;
				rcvnum += packetSize;
				
				if((start == 0) && (rcvnum > (DATASIZE))){
					Serial.println("RCV START !");
					tskstop = 0;
					delay(1);   // タスク開始前に変数を反映する時間を確保
					// 再生タスク
					xTaskCreatePinnedToCore(i2sPlayTask, "i2sPlayTask", 4096, NULL, 1, NULL, 1);
					start = 1;
				}
				if((start == 1) && (rcvnum > (DATASIZE * WAITPACKET))){
					Serial.println("PLAY START !");
					start = 2;
					// Set SPK mode
					InitI2SSpeakerOrMic(MODE_SPK);
				}
			}
			else{
				if(start == 2){
					Serial.printf("[%d] %d(%d)\n", count++, total, packetSize);
					
					start = 0;
					delay(1);  // タスク停止前に変数を反映する時間を確保
					tskstop = 1;
					
					wpos = 0;
					rpos = 0;
					playnum = 0;
					rcvnum = 0;
					total = 0;
					count = 0;
					
					// Set MIC mode
					InitI2SSpeakerOrMic(MODE_MIC);
					
					i2s_zero_dma_buffer(SPEAKER_I2S_NUMBER);
					
					Serial.println("PLAY STOP !");
				}
			}
		}
	}
	
	if(WiFi.status() != WL_CONNECTED){
		// WiFi切断 -> リブート
		Serial.println("\nERROR : cannot connect! & reset!");
		resetboard();  // call reset
	}
	
	delay(5);
}
