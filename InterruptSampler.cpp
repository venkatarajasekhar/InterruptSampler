#include "InterruptSampler.h"
#include "Arduino.h"

volatile uint32_t is_now = 0;
volatile uint32_t is_period = 0;
volatile uint8_t is_bit3Sample = 0;
volatile uint32_t is_lastTimeStamp = 0;
volatile uint32_t is_minPeriod = 0xFFFFFFFF;
volatile uint32_t is_maxPeriod = 0;
volatile uint16_t is_currentBit = 0;
volatile uint32_t is_noOfMisfire = 0;
volatile uint32_t is_noOfRestarts = 0;

volatile uint8_t is_bitBuffer[IS_BUFFER_SIZE];
volatile uint32_t is_endOfBlockPeriod = 0xFFFFFFFF;  // This is the "end of block" marker
volatile uint16_t is_bitsToLookFor = 0; 

#ifdef __AVR__
#define PIN3IN ((PIND>>3)&1)
#else
#define PIN3IN digitalRead(3)
#endif

#define printf Serial.print

typedef enum {
  LOOKING_FOR_FIRST_GAP=0,
  LOOKING_FOR_SECOND_GAP=1,
  DONE=2
} SamplerState;

static volatile SamplerState is_samplerState = LOOKING_FOR_FIRST_GAP;

uint32_t is_assembleResult(int fromBit, int toBit, bool duplicateHighBit) {
  // non byte aligned slice - slow and inefficient
  
  if (toBit >= is_currentBit) {
    toBit = is_currentBit-1;
  }
  bool bitValue = false;
  uint32_t tmp = 0;
  if (fromBit <= toBit) {
    if (toBit - fromBit > 32) {
      printf("Error: toBit - fromBit > 32");
      return tmp;
    }
    int aBit=0;
    for (aBit=toBit; aBit>=fromBit; aBit--){
      bitValue = ((is_bitBuffer[(aBit>>3)]) >> ( (aBit & 0x7)) & 1);
      if (bitValue) {
        tmp |= bit(aBit-fromBit);
      } else {
        tmp &= ~bit(aBit-fromBit);
      }
    }
  } else {
    printf("fromBit <= toBit: not implemented\n");
  }
  if (duplicateHighBit && (tmp & bit(toBit-fromBit))) {
    tmp |= 0xFFFFFFFF << (toBit-fromBit);
  }
  return tmp;
}

void is_clearBuffer(void){
  for (int i=0; i<IS_BUFFER_SIZE; i++) {
   is_bitBuffer[i] = 0;
  }
}

/**
 * prints the bits [fromBit ... untilBit] (inclusive)
 * as "1" and "0" on the serial port
 */
void is_printBuffer(uint16_t fromBit, uint16_t untilBit) {
  if ((untilBit>>3) > IS_BUFFER_SIZE) {
    Serial.print("Bit "); Serial.print(untilBit); Serial.println("won't fit the buffer");
  }
  int aByte = 0;
  int aBit = 0;
  for (int i=fromBit; i<=untilBit; i++) {
    aByte = i >> 3;
    aBit = i & 0x7;
    Serial.print(is_bitBuffer[i]&bit(aBit)?"1":"0");
    
    if(i%64==0){
      Serial.println();
    } else if(aBit==0){
      Serial.print(",");
    } 
    
  }
  Serial.println();
}

void is_printBinary(uint32_t data, int untilBit){
  for (int j=untilBit; j>=0;j--){
    Serial.print((data>>j)&1?"1":"0");
    if (j!=untilBit && (j%8)==0) {
      Serial.print(" ");
    }
  }
}

void is_printBinary8(uint8_t data) {
  is_printBinary(data, 7);
}

void is_printBinary16(uint16_t data){
  is_printBinary(data, 15);
}

void is_printBinary32(uint32_t data){
  is_printBinary(data, 31);
}

/**
 * Stored the bit value of 'is_bit3Sample' in 'is_bitBuffer' at position 'is_currentBit'
 * increments is_currentBit.
 */
void is_storeBit(void) {
  int aByte = is_currentBit >> 3;
  int aBit = is_currentBit & 0x7;
  
  if (is_bit3Sample) {
    // set the sample bit
    is_bitBuffer[aByte] |= bit(aBit);
  } else {
    // clear the sample bit (and the bits above as well)
    is_bitBuffer[aByte] &= 0xFF>>(8-aBit);
  }
#if defined (__arm__) && defined (__SAM3X8E__) // Arduino Due compatible
  is_currentBit = (is_currentBit+1)&0x1FFF; // 8192 bits
#else
  is_currentBit = (is_currentBit+1)&0xFFF; // 4096 bits
#endif
}

void is_savePeriodStatistics(void) {
  if (is_period < is_minPeriod) {
    is_minPeriod = is_period;
  }
  
  if (is_period > is_maxPeriod) {
    is_maxPeriod = is_period;
  }
} 

/**
 * does not care about samplerState
 * Just calculates the min and max period of the clock
 */
void is_sampleInterrupt1(void) {
  is_now = micros();  
  is_period = is_now - is_lastTimeStamp;
  is_savePeriodStatistics();
  is_lastTimeStamp = is_now;
}

/**
 * Calculates the number of bits between each "end of block period"
 */
void is_sampleInterrupt2(void) {
  // get the time and data as quickly as possible
  is_bit3Sample = PIN3IN;
  is_now = micros();
  
  if (DONE == is_samplerState) {
    is_noOfMisfire += 1;
    return; // Should not happend
  }
  is_period = is_now - is_lastTimeStamp;
  
  if (LOOKING_FOR_FIRST_GAP == is_samplerState) {
    if (is_period > is_endOfBlockPeriod){
      is_samplerState = LOOKING_FOR_SECOND_GAP;
      is_storeBit();
    }
  } else if (LOOKING_FOR_SECOND_GAP == is_samplerState) {
    if (is_period > is_endOfBlockPeriod){
      is_samplerState = DONE;
      is_stopInterrupt();
      
      is_savePeriodStatistics();
      return;
    } else {
      is_storeBit();
    }
  } 
  is_savePeriodStatistics();  
  is_lastTimeStamp = is_now;
}

/**
 * The real sampler, now we know the number of bits to look for.
 * We can just start clock in the bits until we either:
 *   - find all of them -> return
 *   - or enconter an 'end of block' period. Then we just start over.
 */
void is_sampleInterrupt3(void) {
  // get the time and data as quickly as possible
  is_bit3Sample = PIN3IN;
  is_now = micros();
  
  if (DONE == is_samplerState) {
    is_noOfMisfire += 1;
    return; // Should not happend
  }
  is_period = is_now - is_lastTimeStamp;
  
  if (is_period > is_endOfBlockPeriod){
    // Start over
    is_currentBit = 0;
    is_storeBit();
    is_noOfRestarts += 1;
  } else {
    is_storeBit();
    if (is_bitsToLookFor <= is_currentBit) {
      is_stopInterrupt();
    }
  } 
  is_lastTimeStamp = is_now;
}

/**
 * Another 'real' sampler, now we know the number of bits to look for.
 * And the idle period. 
 * This sampler will first wait for the 'idle period' then start sampling.
 *
 */
void is_sampleInterrupt4(void) {
  // get the time and data as quickly as possible
  is_bit3Sample = PIN3IN;
  is_now = micros();
  
  if (DONE == is_samplerState) {
    is_noOfMisfire += 1;
    return; // Should not happend
  }
  is_period = is_now - is_lastTimeStamp;
  
  if (LOOKING_FOR_FIRST_GAP == is_samplerState) {
    if (is_period > is_endOfBlockPeriod){
      is_samplerState = LOOKING_FOR_SECOND_GAP;
      is_storeBit();
    }
  } else if (LOOKING_FOR_SECOND_GAP == is_samplerState) {
    if (is_period > is_endOfBlockPeriod){
      // we must have missed a bit, Start over
      is_currentBit = 0;
      is_noOfRestarts += 1;
      is_storeBit();
    } else {
      is_storeBit();
      if(is_bitsToLookFor <= is_currentBit) {
        is_stopInterrupt(); 
      }
    }
  } 
  is_lastTimeStamp = is_now;
}

bool is_isDone(void) {
  return DONE == is_samplerState;
}

void is_printState(void) {
  switch (is_samplerState) {
    case LOOKING_FOR_FIRST_GAP:
      Serial.println(" LOOKING_FOR_FIRST_GAP");
      break;
    case LOOKING_FOR_SECOND_GAP:
      Serial.println(" LOOKING_FOR_SECOND_GAP");
      break;
    case DONE:
      Serial.println(" DONE");
      break;
    default:
      Serial.println(" default");
  }
}

void is_printSampleStatistics(void){
  Serial.print("Max period = "); Serial.print(is_maxPeriod); Serial.println();
  Serial.print("Min period = "); Serial.print(is_minPeriod); Serial.println();
  Serial.print("Number of misfires = "); Serial.print(is_noOfMisfire); Serial.println();
}

void is_startInterrupt1(bool onRising) {
  is_maxPeriod = 0;
  is_minPeriod = 0xFFFFFFFF;
  is_currentBit = 0;
  is_samplerState = LOOKING_FOR_FIRST_GAP;
  is_lastTimeStamp = micros();
  is_noOfMisfire = 0;
  is_noOfRestarts = 0;
#if defined (__arm__) && defined (__SAM3X8E__) // Arduino Due compatible
  attachInterrupt(2, is_sampleInterrupt1, onRising?RISING:FALLING); // 2 is pin number
#else
  attachInterrupt(0, is_sampleInterrupt1, onRising?RISING:FALLING);
#endif
}


void is_startInterrupt2(uint32_t endOfBlockPeriod, bool onRising) {
  is_endOfBlockPeriod = endOfBlockPeriod;
  is_maxPeriod = 0;
  is_minPeriod = 0xFFFFFFFF;
  is_currentBit = 0;
  is_samplerState = LOOKING_FOR_FIRST_GAP;
  is_lastTimeStamp = micros();
  is_noOfMisfire = 0;
  is_noOfRestarts = 0;
#if defined (__arm__) && defined (__SAM3X8E__) // Arduino Due compatible  
  attachInterrupt(2, is_sampleInterrupt2, onRising?RISING:FALLING); // 2 is pin number
#else
  attachInterrupt(0, is_sampleInterrupt2, onRising?RISING:FALLING);
#endif
}

void is_startInterrupt3(uint32_t endOfBlockPeriod, uint16_t bitsToLookFor, bool onRising) {
  is_bitsToLookFor = bitsToLookFor;
  is_endOfBlockPeriod = endOfBlockPeriod;
  is_maxPeriod = 0;
  is_minPeriod = 0xFFFFFFFF;
  is_currentBit = 0;
  is_samplerState = LOOKING_FOR_SECOND_GAP;
  is_lastTimeStamp = micros();
  is_noOfMisfire = 0;
  is_noOfRestarts = 0;
#if defined (__arm__) && defined (__SAM3X8E__) // Arduino Due compatible
  attachInterrupt(2, is_sampleInterrupt3, onRising?RISING:FALLING); // 2 is pin number
#else
  attachInterrupt(0, is_sampleInterrupt3, onRising?RISING:FALLING);
#endif
}

void is_startInterrupt4(uint32_t endOfBlockPeriod, uint16_t bitsToLookFor, bool onRising) {
  is_bitsToLookFor = bitsToLookFor;
  is_endOfBlockPeriod = endOfBlockPeriod;
  is_maxPeriod = 0;
  is_minPeriod = 0xFFFFFFFF;
  is_currentBit = 0;
  is_samplerState = LOOKING_FOR_FIRST_GAP;
  is_lastTimeStamp = micros();
  is_noOfMisfire = 0;
  is_noOfRestarts = 0;
#if defined (__arm__) && defined (__SAM3X8E__) // Arduino Due compatible
  attachInterrupt(2, is_sampleInterrupt4, onRising?RISING:FALLING); // 2 is pin number
#else
  attachInterrupt(0, is_sampleInterrupt4, onRising?RISING:FALLING);
#endif
}

void is_stopInterrupt(void) {
#if defined (__arm__) && defined (__SAM3X8E__) // Arduino Due compatible
  detachInterrupt(2); // 2 is pin number
#else
  detachInterrupt(0);
#endif
  is_samplerState = DONE;
}
