#include "wled.h"
#include <driver/i2s.h>
#include "sound_reactive.h"

/****************** UDP SYNC DEFINITIONS ******************/

#define UDP_SYNC_HEADER "00001"

struct audioSyncPacket {
  char header[6] = UDP_SYNC_HEADER;
  uint8_t myVals[32];     //  32 Bytes
  int sampleAgc;          //  04 Bytes
  int sample;             //  04 Bytes
  float sampleAvg;        //  04 Bytes
  bool samplePeak;        //  01 Bytes
  uint8_t fftResult[16];  //  16 Bytes
  double FFT_Magnitude;   //  08 Bytes
  double FFT_MajorPeak;   //  08 Bytes
};

/********************* UDP SYNC CODE **********************/

bool isValidUdpSyncVersion(char header[6]) {
  if (strncmp(header, UDP_SYNC_HEADER, 6) == 0) {
    return true;
  } else {
    return false;
  }
}

void transmitAudioData() {
  if (!udpSyncConnected) return;
  extern uint8_t myVals[];
  extern int sampleAgc;
  extern int sample;
  extern float sampleAvg;
  extern bool udpSamplePeak;
  extern int fftResult[];
  extern double FFT_Magnitude;
  extern double FFT_MajorPeak;

  audioSyncPacket transmitData;

  for (int i = 0; i < 32; i++) {
    transmitData.myVals[i] = myVals[i];
  }

  transmitData.sampleAgc = sampleAgc;
  transmitData.sample = sample;
  transmitData.sampleAvg = sampleAvg;
  transmitData.samplePeak = udpSamplePeak;
  udpSamplePeak = 0;                            // Reset udpSamplePeak after we've transmitted it

  for (int i = 0; i < 16; i++) {
    transmitData.fftResult[i] = (uint8_t)constrain(fftResult[i], 0, 254);
  }

  transmitData.FFT_Magnitude = FFT_Magnitude;
  transmitData.FFT_MajorPeak = FFT_MajorPeak;

  fftUdp.beginMulticastPacket();
  fftUdp.write(reinterpret_cast<uint8_t *>(&transmitData), sizeof(transmitData));
  fftUdp.endPacket();
  return;
} // transmitAudioData()

/***************** SHARED AUDIO VARIABLES *****************/

// TODO: put comments in one place only so they don't get out of sync as they change

uint8_t myVals[32];                             // Used to store a pile of samples because WLED frame rate and WLED sample rate are not synchronized. Frame rate is too low.
int sample;                                     // Current sample. Must only be updated ONCE!!!
int sampleAgc;                                  // Our AGC sample
bool samplePeak = 0;                            // Boolean flag for peak. Responding routine must reset this flag
float sampleAvg = 0;                            // Smoothed Average
double FFT_Magnitude = 0;                       // Same here. Not currently used though
double FFT_MajorPeak = 0;                       // Optional inclusion for our volume routines
uint16_t mAvg = 0;

// Try and normalize fftBin values to a max of 4096, so that 4096/16 = 256.
// Oh, and bins 0,1,2 are no good, so we'll zero them out.
double fftBin[samples];                 // raw FFT data
int fftResult[16];                      // summary of bins array. 16 summary bins.

/************ SAMPLING AND FFT LOCAL VARIABLES ************/

uint8_t binNum;                                 // Used to select the bin for FFT based beat detection
uint8_t maxVol = 10;                            // Reasonable value for constant volume for 'peak detector', as it won't always trigger
uint8_t targetAgc = 60;                         // This is our setPoint at 20% of max for the adjusted output
bool udpSamplePeak = 0;                         // Boolean flag for peak. Set at the same tiem as samplePeak, but reset by transmitAudioData
int delayMs = 10;                               // I don't want to sample too often and overload WLED
int micIn;                                      // Current sample starts with negative values and large values, which is why it's 16 bit signed
int sampleAdj;                                  // Gain adjusted sample value
int tmpSample;                                  // An interim sample variable used for calculatioins
uint16_t micData;                               // Analog input for FFT
uint16_t micDataSm;                             // Smoothed mic data, as it's a bit twitchy
long lastTime = 0;
long timeOfPeak = 0;
float expAdjF;                                  // Used for exponential filter.
float micLev = 0;                               // Used to convert returned value to have '0' as minimum. A leveller
float multAgc;                                  // sample * multAgc = sampleAgc. Our multiplier
float weighting = 0.2;                          // Exponential filter weighting. Will be adjustable in a future release.
double beat = 0;                                // beat Detection

/************* SAMPLING AND FFT CODE ************/

#include "arduinoFFT.h"

const i2s_port_t I2S_PORT = I2S_NUM_0;
const int BLOCK_SIZE = 64;
const int SAMPLE_RATE = 10240;                  // Base sample rate in Hz

unsigned int sampling_period_us;
unsigned long microseconds;

// These are the input and output vectors.  Input vectors receive computed results from FFT.
double vReal[samples];
double vImag[samples];

double fftCalc[16];
double fftResultMax[16];                // A table used for testing to determine how our post-processing is working.
float fftAvg[16];

// Table of linearNoise results to be multiplied by soundSquelch in order to reduce squelch across fftResult bins.
int linearNoise[16] = { 34, 28, 26, 25, 20, 12, 9, 6, 4, 4, 3, 2, 2, 2, 2, 2 };

// Table of multiplication factors so that we can even out the frequency response.
double fftResultPink[16] = {1.70,1.71,1.73,1.78,1.68,1.56,1.55,1.63,1.79,1.62,1.80,2.06,2.47,3.35,6.83,9.55};

/*
 * A simple averaging multiplier to automatically adjust sound sensitivity.
 */
void agcAvg() {

  multAgc = (sampleAvg < 1) ? targetAgc : targetAgc / sampleAvg;  // Make the multiplier so that sampleAvg * multiplier = setpoint
  int tmpAgc = sample * multAgc;
  if (tmpAgc > 255) tmpAgc = 0;
  sampleAgc = tmpAgc;                             // ONLY update sampleAgc ONCE because it's used elsewhere asynchronously!!!!
  userVar0 = sampleAvg * 4;
  if (userVar0 > 255) userVar0 = 255;
} // agcAvg()

// Create FFT object
arduinoFFT FFT = arduinoFFT( vReal, vImag, samples, SAMPLE_RATE );

double fftAdd( int from, int to) {
  int i = from;
  double result = 0;
  while ( i <= to) {
    result += fftBin[i++];
  }
  return result;
}

// FFT main code
void FFTcode( void * parameter) {
  DEBUG_PRINT("FFT running on core: "); DEBUG_PRINTLN(xPortGetCoreID());
  //double beatSample = 0;  // COMMENTED OUT - UNUSED VARIABLE COMPILER WARNINGS
  //double envelope = 0;    // COMMENTED OUT - UNUSED VARIABLE COMPILER WARNINGS

  for(;;) {
    delay(1);           // DO NOT DELETE THIS LINE! It is needed to give the IDLE(0) task enough time and to keep the watchdog happy.
                        // taskYIELD(), yield(), vTaskDelay() and esp_task_wdt_feed() didn't seem to work.

    // Only run the FFT computing code if we're not in Receive mode
    if (audioSyncEnabled & (1 << 1))
      continue;

    microseconds = micros();
    //extern double volume;   // COMMENTED OUT - UNUSED VARIABLE COMPILER WARNINGS

    for(int i=0; i<samples; i++) {
      if ((digitalMic && dmEnabled) == false) {
        micData = analogRead(audioPin);           // Analog Read
      } else {
        int32_t digitalSample = 0;
// TODO: I2S_POP_SAMLE DEPRECATED, FIND ALTERNATE SOLUTION
        int bytes_read = i2s_pop_sample(I2S_PORT, (char *)&digitalSample, portMAX_DELAY); // no timeout
        if (bytes_read > 0) {
          micData = abs(digitalSample >> 16);
        }
      }
      micDataSm = ((micData * 3) + micData)/4;    // We'll be passing smoothed micData to the volume routines as the A/D is a bit twitchy.
      vReal[i] = micData;                         // Store Mic Data in an array
      vImag[i] = 0;

      // MIC DATA DEBUGGING
      // DEBUGSR_PRINT("micData: ");
      // DEBUGSR_PRINT(micData);
      // DEBUGSR_PRINT("\tmicDataSm: ");
      // DEBUGSR_PRINT("\t");
      // DEBUGSR_PRINT(micDataSm);
      // DEBUGSR_PRINT("\n");

      if ((digitalMic && dmEnabled) == false) { while(micros() - microseconds < sampling_period_us){/*empty loop*/} }

      microseconds += sampling_period_us;
    }

    FFT.Windowing( FFT_WIN_TYP_HAMMING, FFT_FORWARD );      // Weigh data
    FFT.Compute( FFT_FORWARD );                             // Compute FFT
    FFT.ComplexToMagnitude();                               // Compute magnitudes

    //
    // vReal[3 .. 255] contain useful data, each a 20Hz interval (60Hz - 5120Hz).
    // There could be interesting data at bins 0 to 2, but there are too many artifacts.
    //
    FFT.MajorPeak(&FFT_MajorPeak, &FFT_Magnitude);          // let the effects know which freq was most dominant

    for (int i = 0; i < samples; i++) {                     // Values for bins 0 and 1 are WAY too large. Might as well start at 3.
      double t = 0.0;
      t = abs(vReal[i]);
      t = t / 16.0;                                         // Reduce magnitude. Want end result to be linear and ~4096 max.
      fftBin[i] = t;
    } // for()


    /* This FFT post processing is a DIY endeavour. What we really need is someone with sound engineering expertise to do a great job here AND most importantly, that the animations look GREAT as a result.
     *
     *
     * Andrew's updated mapping of 256 bins down to the 16 result bins with Sample Freq = 10240, samples = 512 and some overlap.
     * Based on testing, the lowest/Start frequency is 60 Hz (with bin 3) and a highest/End frequency of 5120 Hz in bin 255.
     * Now, Take the 60Hz and multiply by 1.320367784 to get the next frequency and so on until the end. Then detetermine the bins.
     * End frequency = Start frequency * multiplier ^ 16
     * Multiplier = (End frequency/ Start frequency) ^ 1/16
     * Multiplier = 1.320367784
     */

                                          // Range
    fftCalc[0] = (fftAdd(3,4)) /2;        // 60 - 100
    fftCalc[1] = (fftAdd(4,5)) /2;        // 80 - 120
    fftCalc[2] = (fftAdd(5,7)) /3;        // 100 - 160
    fftCalc[3] = (fftAdd(7,9)) /3;        // 140 - 200
    fftCalc[4] = (fftAdd(9,12)) /4;       // 180 - 260
    fftCalc[5] = (fftAdd(12,16)) /5;      // 240 - 340
    fftCalc[6] = (fftAdd(16,21)) /6;      // 320 - 440
    fftCalc[7] = (fftAdd(21,28)) /8;      // 420 - 600
    fftCalc[8] = (fftAdd(29,37)) /10;     // 580 - 760
    fftCalc[9] = (fftAdd(37,48)) /12;     // 740 - 980
    fftCalc[10] = (fftAdd(48,64)) /17;    // 960 - 1300
    fftCalc[11] = (fftAdd(64,84)) /21;    // 1280 - 1700
    fftCalc[12] = (fftAdd(84,111)) /28;   // 1680 - 2240
    fftCalc[13] = (fftAdd(111,147)) /37;  // 2220 - 2960
    fftCalc[14] = (fftAdd(147,194)) /48;  // 2940 - 3900
    fftCalc[15] = (fftAdd(194, 255)) /62; // 3880 - 5120

    // Noise supression of fftCalc bins using soundSquelch adjustment for different input types.
    for (int i=0; i < 16; i++) {
        fftCalc[i] = fftCalc[i]-(float)soundSquelch*(float)linearNoise[i]/4.0 <= 0? 0 : fftCalc[i];
    }

    // Adjustment for frequency curves.
    for (int i=0; i < 16; i++) {
      fftCalc[i] = fftCalc[i] * fftResultPink[i];
    }

    // Manual linear adjustment of gain using sampleGain adjustment for different input types.
    for (int i=0; i < 16; i++) {
        fftCalc[i] = fftCalc[i] * sampleGain / 40 + fftCalc[i]/16.0;
    }

    // Now, let's dump it all into fftResult. Need to do this, otherwise other routines might grab fftResult values prematurely.
    for (int i=0; i < 16; i++) {
        // fftResult[i] = (int)fftCalc[i];
        fftResult[i] = constrain((int)fftCalc[i],0,254);
        fftAvg[i] = (float)fftResult[i]*.05 + (1-.05)*fftAvg[i];
    }

// Looking for fftResultMax for each bin using Pink Noise
//      for (int i=0; i<16; i++) {
//          fftResultMax[i] = ((fftResultMax[i] * 63.0) + fftResult[i]) / 64.0;
//         Serial.print(fftResultMax[i]*fftResultPink[i]); Serial.print("\t");
//        }
//      Serial.println(" ");

  } // for(;;)
} // FFTcode()

void getSample() {
  static long peakTime;
  //extern double FFT_Magnitude;                    // Optional inclusion for our volume routines // COMMENTED OUT - UNUSED VARIABLE COMPILER WARNINGS
  //extern double FFT_MajorPeak;                    // Same here. Not currently used though       // COMMENTED OUT - UNUSED VARIABLE COMPILER WARNINGS

  #ifdef WLED_DISABLE_SOUND
    micIn = inoise8(millis(), millis());          // Simulated analog read
  #else
    micIn = micDataSm;      // micDataSm = ((micData * 3) + micData)/4;
/*---------DEBUG---------*/
    DEBUGSR_PRINT("micIn:\tmicData:\tmicIn>>2:\tmic_In_abs:\tsample:\tsampleAdj:\tsampleAvg:\n");
    DEBUGSR_PRINT(micIn); DEBUGSR_PRINT("\t"); DEBUGSR_PRINT(micData);
/*-------END DEBUG-------*/
// We're still using 10 bit, but changing the analog read resolution in usermod.cpp
//    if (digitalMic == false) micIn = micIn >> 2;  // ESP32 has 2 more bits of A/D than ESP8266, so we need to normalize to 10 bit.
/*---------DEBUG---------*/
    DEBUGSR_PRINT("\t\t"); DEBUGSR_PRINT(micIn);
/*-------END DEBUG-------*/
  #endif
  micLev = ((micLev * 31) + micIn) / 32;          // Smooth it out over the last 32 samples for automatic centering
  micIn -= micLev;                                // Let's center it to 0 now
  micIn = abs(micIn);                             // And get the absolute value of each sample
/*---------DEBUG---------*/
  DEBUGSR_PRINT("\t\t"); DEBUGSR_PRINT(micIn);
/*-------END DEBUG-------*/

// Using an exponential filter to smooth out the signal. We'll add controls for this in a future release.
  expAdjF = (weighting * micIn + (1.0-weighting) * expAdjF);
  expAdjF = (expAdjF <= soundSquelch) ? 0: expAdjF;

  tmpSample = (int)expAdjF;

/*---------DEBUG---------*/
  DEBUGSR_PRINT("\t\t"); DEBUGSR_PRINT(sample);
/*-------END DEBUG-------*/

  sampleAdj = tmpSample * sampleGain / 40 + tmpSample / 16; // Adjust the gain.
  sampleAdj = min(sampleAdj, 255);
  sample = sampleAdj;                             // ONLY update sample ONCE!!!!

  sampleAvg = ((sampleAvg * 15) + sample) / 16;   // Smooth it out over the last 16 samples.

/*---------DEBUG---------*/
  DEBUGSR_PRINT("\t"); DEBUGSR_PRINT(sample);
  DEBUGSR_PRINT("\t\t"); DEBUGSR_PRINT(sampleAvg); DEBUGSR_PRINT("\n\n");
/*-------END DEBUG-------*/

  if (millis() - timeOfPeak > MIN_SHOW_DELAY) {   // Auto-reset of samplePeak after a complete frame has passed.
    samplePeak = 0;
    udpSamplePeak = 0;
    }

  if (userVar1 == 0) samplePeak = 0;
  // Poor man's beat detection by seeing if sample > Average + some value.
  //  Serial.print(binNum); Serial.print("\t"); Serial.print(fftBin[binNum]); Serial.print("\t"); Serial.print(fftAvg[binNum/16]); Serial.print("\t"); Serial.print(maxVol); Serial.print("\t"); Serial.println(samplePeak);
    if (fftBin[binNum] > ( maxVol) && millis() > (peakTime + 100)) {                     // This goe through ALL of the 255 bins
  //  if (sample > (sampleAvg + maxVol) && millis() > (peakTime + 200)) {
  // Then we got a peak, else we don't. The peak has to time out on its own in order to support UDP sound sync.
    samplePeak = 1;
    timeOfPeak = millis();
    udpSamplePeak = 1;
    userVar1 = samplePeak;
    peakTime=millis();
  }
} // getSample()

void logAudio() {
#ifdef MIC_LOGGER
  //  Serial.print(micIn);      Serial.print(" ");
  //  Serial.print(sample); Serial.print(" ");
  //  Serial.print(sampleAvg); Serial.print(" ");
  //  Serial.print(sampleAgc);  Serial.print(" ");
  //  Serial.print(micData);    Serial.print(" ");
  //  Serial.print(micDataSm);  Serial.print(" ");
  Serial.println(" ");
#endif

#ifdef MIC_SAMPLING_LOG
  //------------ Oscilloscope output ---------------------------
  Serial.print(targetAgc); Serial.print(" ");
  Serial.print(multAgc); Serial.print(" ");
  Serial.print(sampleAgc); Serial.print(" ");

  Serial.print(sample); Serial.print(" ");
  Serial.print(sampleAvg); Serial.print(" ");
  Serial.print(micLev); Serial.print(" ");
  Serial.print(samplePeak); Serial.print(" ");    //samplePeak = 0;
  Serial.print(micIn); Serial.print(" ");
  Serial.print(100); Serial.print(" ");
  Serial.print(0); Serial.print(" ");
  Serial.println(" ");
#endif

#ifdef FFT_SAMPLING_LOG
  #if 0
    for(int i=0; i<16; i++) {
      Serial.print(fftResult[i]);
      Serial.print("\t");
    }
    Serial.println("");
  #endif

  // OPTIONS are in the following format: Description \n Option
  //
  // Set true if wanting to see all the bands in their own vertical space on the Serial Plotter, false if wanting to see values in Serial Monitor
  const bool mapValuesToPlotterSpace = false;
  // Set true to apply an auto-gain like setting to to the data (this hasn't been tested recently)
  const bool scaleValuesFromCurrentMaxVal = false;
  // prints the max value seen in the current data
  const bool printMaxVal = false;
  // prints the min value seen in the current data
  const bool printMinVal = false;
  // if !scaleValuesFromCurrentMaxVal, we scale values from [0..defaultScalingFromHighValue] to [0..scalingToHighValue], lower this if you want to see smaller values easier
  const int defaultScalingFromHighValue = 256;
  // Print values to terminal in range of [0..scalingToHighValue] if !mapValuesToPlotterSpace, or [(i)*scalingToHighValue..(i+1)*scalingToHighValue] if mapValuesToPlotterSpace
  const int scalingToHighValue = 256;
  // set higher if using scaleValuesFromCurrentMaxVal and you want a small value that's also the current maxVal to look small on the plotter (can't be 0 to avoid divide by zero error)
  const int minimumMaxVal = 1;

  int maxVal = minimumMaxVal;
  int minVal = 0;
  for(int i = 0; i < 16; i++) {
    if(fftResult[i] > maxVal) maxVal = fftResult[i];
    if(fftResult[i] < minVal) minVal = fftResult[i];
  }
  for(int i = 0; i < 16; i++) {
    Serial.print(i); Serial.print(":");
    Serial.printf("%04d ", map(fftResult[i], 0, (scaleValuesFromCurrentMaxVal ? maxVal : defaultScalingFromHighValue), (mapValuesToPlotterSpace*i*scalingToHighValue)+0, (mapValuesToPlotterSpace*i*scalingToHighValue)+scalingToHighValue-1));
  }
  if(printMaxVal) {
    Serial.printf("maxVal:%04d ", maxVal + (mapValuesToPlotterSpace ? 16*256 : 0));
  }
  if(printMinVal) {
    Serial.printf("%04d:minVal ", minVal);  // printed with value first, then label, so negative values can be seen in Serial Monitor but don't throw off y axis in Serial Plotter
  }
  if(mapValuesToPlotterSpace)
    Serial.printf("max:%04d ", (printMaxVal ? 17 : 16)*256); // print line above the maximum value we expect to see on the plotter to avoid autoscaling y axis
  else
    Serial.printf("max:%04d ", 256);
  Serial.println();
#endif // FFT_SAMPLING_LOG
} // logAudio()

/**************** USERMOD V2 CODE ***************/

// usermodv2 setup - setup() is called once at boot. WiFi is not yet connected at this point.
void SoundreactiveUsermod::setup() {
  delay(100);                                   // Give that poor microphone some time to setup.
  // Attempt to configure INMP441 Microphone
  esp_err_t err;
  const i2s_config_t i2s_config = {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX),  // Receive, not transfer
    .sample_rate = SAMPLE_RATE*2,                       // 10240 * 2 (20480) Hz
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,       // could only get it to work with 32bits
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,        // LEFT when pin is tied to ground.
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,           // Interrupt level 1
    .dma_buf_count = 8,                                 // number of buffers
    .dma_buf_len = BLOCK_SIZE                           // samples per buffer
  };
  const i2s_pin_config_t pin_config = {
    .bck_io_num = i2sckPin,     // BCLK aka SCK
    .ws_io_num = i2swsPin,      // LRCL aka WS
    .data_out_num = -1,         // not used (only for speakers)
    .data_in_num = i2ssdPin     // DOUT aka SD
  };
  // Configuring the I2S driver and pins.
  // This function must be called before any I2S driver read/write operations.
  err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("Failed installing driver: %d\n", err);
    while (true);
  }
  err = i2s_set_pin(I2S_PORT, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("Failed setting pin: %d\n", err);
    while (true);
  }
  Serial.println("I2S driver installed.");
  delay(250);

  // Test to see if we have a digital microphone installed or not.
  float mean = 0.0;
  int32_t samples[BLOCK_SIZE];
// TODO: I2S_READ_BYTES DEPRECATED, FIND ALTERNATE SOLUTION
  int num_bytes_read = i2s_read_bytes(I2S_PORT,
                                      (char *)samples,
                                      BLOCK_SIZE,     // the doc says bytes, but its elements.
                                      portMAX_DELAY); // no timeout

  int samples_read = num_bytes_read / 8;
  if (samples_read > 0) {
    for (int i = 0; i < samples_read; ++i) {
      mean += samples[i];
    }
    mean = mean/BLOCK_SIZE/16384;
    if (mean != 0.0) {
      Serial.println("Digital microphone is present.");
      digitalMic = true;
    } else {
      Serial.println("Digital microphone is NOT present.");
      analogReadResolution(10);          // Default is 12, which is less linear. We're also only using 10 bits as a result of our ESP8266 history.
    }
  }

  pinMode(LED_BUILTIN, OUTPUT);

  sampling_period_us = round(1000000*(1.0/SAMPLE_RATE));

  // Define the FFT Task and lock it to core 0
  xTaskCreatePinnedToCore(
        FFTcode,                          // Function to implement the task
        "FFT",                            // Name of the task
        10000,                            // Stack size in words
        NULL,                             // Task input parameter
        1,                                // Priority of the task
        &FFT_Task,                        // Task handle
        0);                               // Core where the task should run
}

// usermodv2 loop - loop() is called continuously. Here you can check for events, read sensors, etc.
void SoundreactiveUsermod::loop() {

  if (!(audioSyncEnabled & (1 << 1))) { // Only run the sampling code IF we're not in Receive mode
    lastTime = millis();
    getSample();                        // Sample the microphone
    agcAvg();                           // Calculated the PI adjusted value as sampleAvg
    myVals[millis()%32] = sampleAgc;
    logAudio();
  }
  if (audioSyncEnabled & (1 << 0)) {    // Only run the transmit code IF we're in Transmit mode
    //Serial.println("Transmitting UDP Mic Packet");
    EVERY_N_MILLIS(20) {
      transmitAudioData();
    }

  }

  // Begin UDP Microphone Sync
  if (audioSyncEnabled & (1 << 1)) {    // Only run the audio listener code if we're in Receive mode
    if (millis()-lastTime > delayMs) {
      if (udpSyncConnected) {
        //Serial.println("Checking for UDP Microphone Packet");
        int packetSize = fftUdp.parsePacket();
        if (packetSize) {
          // Serial.println("Received UDP Sync Packet");
          uint8_t fftBuff[packetSize];
          fftUdp.read(fftBuff, packetSize);
          audioSyncPacket receivedPacket;
          memcpy(&receivedPacket, fftBuff, packetSize);
          for (int i = 0; i < 32; i++ ){
            myVals[i] = receivedPacket.myVals[i];
          }
          sampleAgc = receivedPacket.sampleAgc;
          sample = receivedPacket.sample;
          sampleAvg = receivedPacket.sampleAvg;
          // VERIFY THAT THIS IS A COMPATIBLE PACKET
          char packetHeader[6];
          memcpy(&receivedPacket, packetHeader, 6);
          if (!(isValidUdpSyncVersion(packetHeader))) {
            memcpy(&receivedPacket, fftBuff, packetSize);
            for (int i = 0; i < 32; i++ ){
              myVals[i] = receivedPacket.myVals[i];
            }
            sampleAgc = receivedPacket.sampleAgc;
            sample = receivedPacket.sample;
            sampleAvg = receivedPacket.sampleAvg;

            // Only change samplePeak IF it's currently false.
            // If it's true already, then the animation still needs to respond.
            if (!samplePeak) {
              samplePeak = receivedPacket.samplePeak;
            }
            //These values are only available on the ESP32
            for (int i = 0; i < 16; i++) {
              fftResult[i] = receivedPacket.fftResult[i];
            }

            FFT_Magnitude = receivedPacket.FFT_Magnitude;
            FFT_MajorPeak = receivedPacket.FFT_MajorPeak;
            //Serial.println("Finished parsing UDP Sync Packet");
          }
        }
      }
    }
  }
} // userLoop()