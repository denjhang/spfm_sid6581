// Written 22-Dec-2011
// Alexis Kotlowy
//
// Released into the Public Domain
//
// Adjust your parameters in the defines below

#include <avr/pgmspace.h>

// Set the volume. Generally is is 15 (maximum)
#define VOLUME 15

// Specify your waveform here
#define WAVEFORM SAWTOOTH


// Specify Pulse Width here (0 to 4095)
// 2048 makes a square wave
#define PULSEW   2048

// Specify ADSR here (0 to 15)
#define ATTACK  0
#define DECAY   6
#define SUSTAIN 9
#define RELEASE 9

// Filter settings. Filters are either enabled (1)
// or disabled (0). Set filter value from 0 to 255.
// Resonance is from 0 to 15.
#define FILTER 1
#define LP 0
#define BP 1
#define HP 0
#define CUTOFF 192
#define RESONANCE 15

// Constants for Waveform
#define TRIANGLE 0x10
#define SAWTOOTH 0x20
#define PULSE    0x40
#define NOISE    0x80

// MIDI signaling constants.
#define NOTEOFF 0x80
#define NOTEON 0x90

// Frequency table for a 1MHz SID
const unsigned int freq[] PROGMEM = { 
  274,  291,  308,  326,  346,  366,  388,  411,  435,  461,  489,
  581,  549,  581,  616,  652,  691,  732,  776,  822,  871,  923,
  976,  1036, 1097, 1163, 1232, 1305, 1383, 1465, 1552, 1644, 1742,
  1845, 1955, 2071, 2195, 2325, 2463, 2610, 2765, 2930, 3104, 3288,
  3484, 3691, 3910, 4143, 4389, 4650, 4927, 5220, 5530, 5859, 6207,
  6577, 6968, 7382, 7821, 8286, 8779, 9301, 9854,10440,11060,11718,
  12415,13153,13935,14764,15642,16572,17557,18601,19709,20897,22121,
  23436,24830,26306,27871,29528,31234,33144,35115,37203,39415,39415,
  41759,44242,46873,49660,52613,55741,59056,62567 };

void initSID() {
  char c;     // For SID clearing loop
  // Place your SID intialisation code here. All three channels should
  // be set to the same values for MIDI to sound right.

  // Clear the SID
  for(c=0; c<25;c++)
    wrSID(c,0);

#if FILTER
  wrSID(22,CUTOFF);     // Set filter value
  wrSID(23,(RESONANCE<<4)|7);        // Set resonance and filtered channels
  wrSID(24, (HP<<6)|(BP<<5)|(LP<<4)|VOLUME);
#else
  wrSID(24,VOLUME);
#endif

  // Set ADSR for three channels
  for(c=0; c<21; c+=7) {
    wrSID(5+c,ATTACK|(DECAY<<4));      // Attack and Decay
    wrSID(6+c,RELEASE|(SUSTAIN<<4));   // Half volume on sustain, medium release
    wrSID(2+c,PULSEW);    // Set pulse width low
    wrSID(3+c,PULSEW>>8); // Set pulse width high
  }
}

void SIDdelay () {
  // Make a really short delay in software to allow for settling
  // time on the bus. The loop needs the NOP command because the
  // compiler optimisations will detect nothing is happening
  // witout it and consequently not put in a loop at all.

  unsigned char i;
  for(i=0;i<10;i++) __asm("nop\n\t");
}

void wrSID (unsigned char port, unsigned char data) {
  // Send byte 'data' to address 'port'.
  // SIDdelay is inserted between operations for settling time. 

  PORTB = (port & 0x0F) | 0x10;  // Lower bit, keep CS deactive
  PORTD = ((port | 0x20) & 0xF0) >> 2; // Upper bit, reset high
  PORTD |= 0x80;    // Clock in Address
  SIDdelay();
  PORTD &= ~(0x80);

  PORTB = (data & 0x0F) | 0x10;  // Lower bit, keep CS deactive
  PORTD = (data & 0xF0) >> 2;    // Upper bit
  PORTB &= ~(0x10);   // Activate /CS
  SIDdelay();
  PORTB |= 0x10;      // Deactivate /CS
}  

void setup () {
  // First, intialise the ports.
  PORTD = 0;
  PORTB = 0x10;  // PB4 is high to disable Chip Select
  DDRD = 0xFE;   // Keep RxD as an input, everything else output
  DDRB = 0xFF;

  // Initialise Timer 0 OC0A to 1MHz on Pin 6.
  TIMSK0 = 0;
  TCNT0 = 0;     // Reset timer
  OCR0A = 0;     // Invert clock after this many clock cycles + 1
  TCCR0A = (1<<COM0A0) | (2<<WGM00);
  TCCR0B = (2<<CS00); // Prescaler

  Serial.begin(31250);    // Initialise serial for MIDI rate
  Serial.flush();

  initSID();

}

void loop () {
  unsigned char n, v, state;  // Note, Velocity and current state
  unsigned char c;            // Count for freq table
  unsigned int f;            // Frequency from table
  unsigned char ntab[3] = {
    0xFF,0xFF,0xFF      };  // Table for channel status
  state = 0x80;
  for (;;) {
    while (Serial.available() == 0);
    n = Serial.read();      // Get next MIDI byte into n
    switch (n & 0x80) {     // Coose if it's a command or operand
    case 0:                 // Operand
      switch (state) {      // Select state specified in last command
      case NOTEON:
        v = Serial.read();  // Get velocity
        if (v) {
          f = pgm_read_word(&freq[n-12]);  // Get frequency of n
          for(c=0;c<3;c++) {               // Scan all channels until available
            if(ntab[c] == 0xFF) {
              wrSID(0+(c*7),f);            // Send frequency to chanel
              wrSID(1+(c*7),f>>8);
              wrSID(4+(c*7),WAVEFORM|1);   // Turn on note with waveform
              ntab[c]=n;
              break;                       // Exit out of seek loop
            }
          }
        } 
        else {                             // If velocity was zero, release
          for(c=0;c<3;c++) {
            if(ntab[c] == n) {             // Search for released note
              wrSID(4+(c*7),WAVEFORM);     // Turn off note on SID
              ntab[c] = 0xFF;              // Set table to reflect change
              break;                       // Exit seek loop
            }
          }
          break;                           // End of NOTEON
        case NOTEOFF:                      // Note released
          v = Serial.read();               // Get velocity (ignored)
          for(c=0;c<3;c++) {
            if(ntab[c] == n) {             // Search for released note
              wrSID(4+(c*7),WAVEFORM);     // Turn off note on SID
              ntab[c] = 0xFF;              // Set table to reflect change
              break;                       // Exit seek loop
            }
          }
          break;                           // End of NOTEOFF
        }
      }
    case 0x80:                             // Set state according to command
      state = n & 0xF0;                    // Mask off channel, responds to all
      break;
    }
  }
}



