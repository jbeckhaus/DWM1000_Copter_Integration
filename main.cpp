#include "mbed.h"
extern "C" {
#include "libdw1000.h"
#include "circular_buffer.h"
}

// ADDR should be same as AC_ID to match telemetry
#define ADDR 2
//#define SWITCH_UART
#define MAGIC_RANGE_OFFSET 153.7
#define PPRZ_MSG_ID 254
#define RANGE_INTERVALL_US 5000
#define TELEMETRY_BAUD 38400
#define DEBUG_BAUD 115200

#define NO_DATA_FRAME_SIZE 4
typedef struct __attribute__((packed, aligned(1))) DataFrame {
    uint8_t src;
    uint8_t dest;
    uint8_t type;
    uint8_t seq; 
    uint8_t data[15];
}DFrame;

/*
 * PPRZ message definition in sw/pprzlink/messages/v1.0/messages.xml
 * <message name="RANGE" id="254">
      <field name="src"                 type="uint8"/>
      <field name="dest"                type="uint8"/>
      <field name="range"               type="double"/>
    </message>
 * */

DigitalOut redLed(LED2);
DigitalOut greenLed(LED1);
SPI spi(SPI_MOSI, SPI_MISO, SPI_SCK);
DigitalOut cs(SPI_CS);
InterruptIn sIRQ(PA_0);
DigitalInOut sReset(PA_1);
#ifdef SWITCH_UART
RawSerial uart1(PA_2, PA_3, TELEMETRY_BAUD);
Serial uart2(PA_9, PA_10, DEBUG_BAUD);
#else
RawSerial uart1(PA_9, PA_10, TELEMETRY_BAUD);
Serial uart2(PA_2, PA_3, DEBUG_BAUD);
#endif
circularBuffer UARTcb;
circularBuffer DWMcb;
uint8_t UARTcb_data[256];
uint8_t DWMcb_data[256];
Mutex DWMMutex; // protect write and receive operations
EventQueue IRQqueue(8 * EVENTS_EVENT_SIZE);
Thread t;
void dwIRQFunction();
void DWMReceive();
void send_pprz_range_message(uint8_t src, uint8_t dest, double range);

/* variables for ranging*/

static const double tsfreq = 499.2e6 * 128; // Timestamp counter frequency
static const double speedOfLight = 299792458.0; // Speed of light in m/s
dwTime_t tStartRound1;
dwTime_t tStartReply1;
dwTime_t tEndReply1;
dwTime_t tEndRound2;
dwTime_t tStartReply2;
dwTime_t tEndReply2;
dwTime_t tDelay;
uint64_t tRound1;
uint64_t tReply1;
uint64_t tRound2;
uint64_t tReply2;
uint8_t knownNodes[32];
double tPropTick;
DFrame txFrame;
DFrame rxFrame;


void calculateDeltaTime(dwTime_t* startTime, dwTime_t* endTime, uint64_t* result){

	uint64_t start = (startTime->full);
	uint64_t end = (endTime->full);

	if(end > start){
		*result = (end - start);
	}
	else{
		*result = (end + (1099511628000-start));
	}
}

void calculatePropagationFormula(const uint64_t& tRound1, const uint64_t& tReply1, const uint64_t& tRound2, const uint64_t& tReply2, double& tPropTick){
	tPropTick = (double)((tRound1 * tRound2) - (tReply1 * tReply2)) / (tRound1 + tReply1 + tRound2 + tReply2);
}

static void spiWrite(dwDevice_t* dev, const void* header, size_t headerLength,
        const void* data, size_t dataLength) {
    cs = 0;
    spi.lock();
    uint8_t* headerP = (uint8_t*) header;
    uint8_t* dataP = (uint8_t*) data;

    for(size_t i = 0; i<headerLength; ++i) {
        spi.write(headerP[i]);
    }
    for(size_t i = 0; i<dataLength; ++i) {
        spi.write(dataP[i]);
    }
    spi.unlock();
    cs = 1;
}

static void spiRead(dwDevice_t* dev, const void *header, size_t headerLength,
        void* data, size_t dataLength) {
    cs = 0;
    spi.lock();
    uint8_t* headerP = (uint8_t*) header;
    uint8_t* dataP = (uint8_t*) data;

    for(size_t i = 0; i<headerLength; ++i) {
        spi.write(headerP[i]);
    }
    for(size_t i = 0; i<dataLength; ++i) {
        dataP[i] = spi.write(0);
    }
    spi.unlock();
    cs = 1;
}

static void spiSetSpeed(dwDevice_t* dev, dwSpiSpeed_t speed)
{
    if (speed == dwSpiSpeedLow)
        spi.frequency(3*1000*1000);

    if (speed == dwSpiSpeedHigh)
        spi.frequency(20*1000*1000);
}

static void reset(dwDevice_t* dev)
{
    sReset.output();
    sReset = 0;
    wait(0.5f);
    sReset.input();
}

static void delayms(dwDevice_t* dev, unsigned int delay)
{
    wait(delay * 0.001f);
}

static dwOps_t ops = {
    .spiRead = spiRead,
    .spiWrite = spiWrite,
    .spiSetSpeed = spiSetSpeed,
    .delayms = delayms,
    .reset = reset
};

dwDevice_t dwm_device;
dwDevice_t* dwm = &dwm_device;

enum FrameType{
    RANGE_0=0,
    RANGE_1=1,
    RANGE_2=2,
    RANGE_3=3,
    RANGE_TRANSFER=4,
    RANGE_DATA=5,
    RANGE_REQUEST=6,
    DATA_FRAME=42,
    PING=254,
    PONG=255
};

void sendDWM(uint8_t* data, int length) {
    DWMMutex.lock();
    spi.lock();
    dwNewTransmit(dwm);
    dwSetData(dwm, data, length);
    dwStartTransmit(dwm);
    spi.unlock();
    DWMMutex.unlock();
}

void send_rp(FrameType type) {
    txFrame.type = type;
    txFrame.src = ADDR;
    txFrame.dest = rxFrame.src;
    txFrame.seq++;
    sendDWM((uint8_t*)&txFrame, NO_DATA_FRAME_SIZE);
}

void register_node() {
    uint8_t addr = rxFrame.src;
    knownNodes[addr/32] |= 1 << (addr % 8);
}

void send_range_transfer() {
    DWMMutex.lock();
    dwGetReceiveTimestamp(dwm, &tEndRound2);
    DWMMutex.unlock();
   	txFrame.type = RANGE_TRANSFER;
    txFrame.src = ADDR;
    txFrame.dest = rxFrame.src;
    txFrame.seq++;
	memcpy(txFrame.data, tStartReply1.raw, 5);
	memcpy((txFrame.data+5), tEndReply1.raw, 5);
	memcpy((txFrame.data+10), tEndRound2.raw, 5);
    sendDWM((uint8_t *)&txFrame, 19);
}

void send_range(double range) {
   	txFrame.type = RANGE_DATA;
    txFrame.src = ADDR;
    txFrame.dest = rxFrame.src;
    txFrame.seq++;
	memcpy(txFrame.data, &range, sizeof(range));
    sendDWM((uint8_t *)&txFrame, NO_DATA_FRAME_SIZE + sizeof(range));
    uart2.printf("%u, %u, %Lf\r\n", txFrame.src, txFrame.dest, range);
     
}

void startRanging() {
    rxFrame.src = 1;
    send_rp(RANGE_0);
}

double calculateDistanceFromTicks(uint64_t tprop) {
    return speedOfLight * tprop / tsfreq - MAGIC_RANGE_OFFSET;
}

double calculate_range() {
   
    DWMMutex.lock();
	dwGetData(dwm, (uint8_t*) &rxFrame, sizeof(rxFrame));
    DWMMutex.unlock();

	memcpy(tStartReply1.raw, rxFrame.data, 5);
	memcpy(tEndReply1.raw, (rxFrame.data+5), 5);
	memcpy(tEndRound2.raw, (rxFrame.data+10), 5);

	calculateDeltaTime(&tStartRound1, &tStartReply2, &tRound1);
	calculateDeltaTime(&tStartReply1, &tEndReply1, &tReply1);
	calculateDeltaTime(&tStartReply2, &tEndReply2, &tReply2);
	calculateDeltaTime(&tEndReply1, &tEndRound2, &tRound2);
	
	calculatePropagationFormula(tRound1, tReply1, tRound2, tReply2, tPropTick);
    return calculateDistanceFromTicks(tPropTick);
}
void DWMReceive() {
    DWMMutex.lock(); 
    dwNewReceive(dwm);
    dwStartReceive(dwm);
    DWMMutex.unlock();
}

void txcallback(dwDevice_t *dev){
    DWMMutex.lock();
    switch(txFrame.type) {
        case RANGE_0:
            dwGetTransmitTimestamp(dev, &tStartRound1);
            break;
        case RANGE_1:
            dwGetReceiveTimestamp(dev, &tStartReply1);
            dwGetTransmitTimestamp(dev, &tEndReply1);
            break;
        case RANGE_2:
            dwGetReceiveTimestamp(dev, &tStartReply2);
            dwGetTransmitTimestamp(dev, &tEndReply2);
            break;
    }
    DWMMutex.unlock();
    DWMReceive();
}
uint8_t Buffer[256+4];
void handle_data_frame() {
    DWMMutex.lock();
    size_t length = dwGetDataLength(dwm);
    // cap length to buffer size
    if(length > sizeof(Buffer)) {
        length = sizeof(Buffer);
    }
    dwGetData(dwm, Buffer, length);
    DWMMutex.unlock();
    // write to the circular buffer, ignoring the 4 header bytes
    circularBuffer_write(&DWMcb, Buffer+4, length-4);
    DWMReceive();
}
void receive_range_answer() {
    double range;
    DWMMutex.lock();
    dwGetData(dwm, (uint8_t*) &rxFrame, NO_DATA_FRAME_SIZE + sizeof(range));
    DWMMutex.unlock();
    memcpy(&range, rxFrame.data, sizeof(range));
    uart2.printf("%u, %u, %Lf\r\n", rxFrame.src, rxFrame.dest, range);
    send_pprz_range_message(rxFrame.src, rxFrame.dest, range);
}

void handle_broadcast_packet() {
    switch(rxFrame.type) {
        case DATA_FRAME:
            handle_data_frame();
            break;
        case PING:
            send_rp(PONG);
            break;
        case PONG:
            register_node();
            DWMReceive();
            break;
        default:
            uart2.printf("unknown frame type\n\r");
            DWMReceive();
            break;
    }
}

void handle_own_packet() {
    switch(rxFrame.type) {
        case RANGE_0:
            send_rp(RANGE_1);
            break;
        case RANGE_1:
            send_rp(RANGE_2);
            break;
        case RANGE_2:
            //send_rp(RANGE_TRANSFER);
            send_range_transfer();
            break;
        case RANGE_TRANSFER: {
            double range = calculate_range();
            send_range(range);
            break;
                             }
        case RANGE_DATA:
            receive_range_answer();
            DWMReceive();
            break;
        default:
            handle_broadcast_packet();
            break;
    }
}

void handle_foreign_packet() {
    switch(rxFrame.type) {
        case RANGE_DATA:
            receive_range_answer();
            DWMReceive();
            break;
        default:
            DWMReceive();
            break;
    }
}

void rxcallback(dwDevice_t *dev)
{ 
    DWMMutex.lock();
    dwGetData(dwm, (uint8_t*) &rxFrame, NO_DATA_FRAME_SIZE);
    DWMMutex.unlock();
    if(rxFrame.src == ADDR) {
        uart2.printf("received own packet - shouldn't happen\r\npossibly the address was given to multiple nodes\r\n\n");
        return;
    }
    switch(rxFrame.dest) {
        case ADDR:
            handle_own_packet();
            break;
        case 0:
            handle_broadcast_packet();
            break;
        case 255:
            handle_broadcast_packet();
            break;
        default:
            handle_foreign_packet();
            break;
    }
    return;
}

void sendUART(uint8_t* data, int length) {
    for(uint8_t i = 0; i<length; i++) {
        uart1.putc(data[i]);
    }
}

void serialRead() {
    // this should collect the packets to be sent, and if an packet is complets, it should be sent ... wow ...
    while(uart1.readable()) {
        circularBuffer_write_element(&UARTcb, uart1.getc());
    }
    greenLed = (circularBuffer_fill(&UARTcb) > 128);
}
void resetRangeVariables() {
    tStartRound1.full = 0;
    tStartReply1.full = 0;
    tEndReply1.full = 0;
    tEndRound2.full = 0;
    tStartReply2.full = 0;
    tEndReply2.full = 0;

    tDelay.full = 0;
    tDelay.full = 74756096;//63897600*5; //5msec 
    rxFrame.type = 0;
    txFrame.type = 0;
}

void dwIRQFunction(){
    dwHandleInterrupt(dwm);
}

void initialiseDWM(void) {
    DWMMutex.lock();
    cs = 1;
    reset(dwm);
    dwInit(dwm, &ops);       // Init libdw
    uint8_t result = dwConfigure(dwm); // Configure the dw1000 chip
    if (result == 0) {
        dwEnableAllLeds(dwm);
    }


    t.start(callback(&IRQqueue, &EventQueue::dispatch_forever));
    t.set_priority(osPriorityHigh);
    dwTime_t delay = {.full = 0};
    dwSetAntenaDelay(dwm, delay);

    dwAttachSentHandler(dwm, txcallback);
    dwAttachReceivedHandler(dwm, rxcallback);
    dwInterruptOnReceived(dwm, true);
    dwInterruptOnSent(dwm, true);

    dwNewConfiguration(dwm);
    dwSetDefaults(dwm);
    dwEnableMode(dwm, MODE_SHORTDATA_MID_ACCURACY);
    dwSetChannel(dwm, CHANNEL_7);
    dwSetPreambleCode(dwm, PREAMBLE_CODE_64MHZ_9);
    dwCommitConfiguration(dwm);
    sIRQ.rise(IRQqueue.event(&dwIRQFunction));
    //dwReceivePermanently(dwm, true);
    resetRangeVariables();

    dwNewReceive(dwm);
    dwSetDefaults(dwm);
    dwStartReceive(dwm);
    DWMMutex.unlock();
    wait(0.5f);
}

void initialiseBuffers(){
    circularBuffer_init(&UARTcb, UARTcb_data, 256);
    circularBuffer_init(&DWMcb, DWMcb_data, 256);
}

uint8_t check_pprz(circularBuffer* cb, size_t i, size_t fill) {
    if(circularBuffer_status(cb) == circularBuffer_EMPTY)
        return 0;
    if(circularBuffer_peek(cb, 0) != 0x99)
        return 0;
    uint8_t l = circularBuffer_peek(cb, 1);
    if(l <= 5) {
        // dismiss packet if length < HEADER + PAYLOAD
        circularBuffer_read_element(cb);
        return 0;
    }
    if(l > fill)
        return 0;
    uint8_t checksumA = 0;
    uint8_t checksumB = 0;
    for(size_t i = 1; i<l-2; i++) {
        checksumA += circularBuffer_peek(cb, i);
        checksumB += checksumA;
    }
    if(checksumA != circularBuffer_peek(cb, l-2)) {
        redLed = !redLed;
        circularBuffer_read_element(cb);
        return 0;
    }
    if(checksumB != circularBuffer_peek(cb, l-1)) {
        redLed = !redLed;
        circularBuffer_read_element(cb);
        return 0;
    }
    return l;
}

uint8_t parsePPRZ(circularBuffer* cb, uint8_t* out) {
    while(1) {
        if(circularBuffer_status(cb) == circularBuffer_EMPTY)
            return 0;
        if(circularBuffer_peek(cb, 0) == 0x99)
            break;
        if(circularBuffer_read_element(cb) == 0x99)
            redLed = !redLed;
    }
    size_t fill = circularBuffer_fill(cb);
    if(fill < 5) {
        return 0;
    } 
    uint8_t l = check_pprz(cb, 0, fill);
    if(l) {
        if(l > fill)
            return 0;
        for(size_t j=0; j<l; j++) {
            out[j] = circularBuffer_read_element(cb);
        }
        return l;
    }
    return 0;
}

/*
 * PPRZ-message: ABCxxxxxxxDE
    A PPRZ_STX (0x99)
    B LENGTH (PPRZ_STX->PPRZ_CHECKSUM_B)
    C PPRZ_DATA
      0 SENDER_ID
      1 MSG_ID
      2 MSG_PAYLOAD
      . DATA (messages.xml)
    D PPRZ_CHECKSUM_A (sum[B->C])
    E PPRZ_CHECKSUM_B (sum[ck_a])

 *
 */
void send_pprz_range_message(uint8_t src, uint8_t dest, double range) {
    uint8_t message[4+2+2+sizeof(range)]; 
    /* ABDE = 4; C0+C1 = 2; C2=2+sizeof(range) */
    uint8_t idx = 0;
    message[idx++] = 0x99; // A
    message[idx++] = sizeof(message); // Room for B
    message[idx++] = src; // C0
    message[idx++] = PPRZ_MSG_ID; // C1
    // C2 Data
    message[idx++] = src; 
    message[idx++] = dest;
    memcpy(&message[idx], &range,  sizeof(range));
    idx += sizeof(range);
    uint8_t checksumA = 0;
    uint8_t checksumB = 0;
    for(uint8_t i = 1; i < idx; i++) {
        checksumA += message[i];
        checksumB += checksumA;
    }
    message[idx++] = checksumA;
    message[idx++] = checksumB;

    sendUART(message, sizeof(message));
}

int main() {
    uart2.printf("Start Ranging\n");
    uart1.attach(&serialRead,Serial::RxIrq);
    initialiseBuffers();
    initialiseDWM();

    uint8_t WriteBuffer[256+4];
#if ADDR != 1
    IRQqueue.call_every(RANGE_INTERVALL_US, startRanging);
#endif
    while (true){
        // write to dwm
        size_t l = parsePPRZ(&UARTcb, WriteBuffer+4);
        if(l){
            WriteBuffer[0] = ADDR;
            WriteBuffer[1] = 0; // Broacast
            WriteBuffer[2] = DATA_FRAME;
            WriteBuffer[3] = txFrame.seq++;
            sendDWM(WriteBuffer, l+4);
        }
        l = parsePPRZ(&DWMcb, WriteBuffer);
        if(l){
            sendUART(WriteBuffer, l);
        }
        wait(0.0001);
    }
}
