// Written 21-Dec-2011
// Alexis Kotlowy
//
// Released into the Public Domain
//
// This example plays a bit of Bach, looped.
// Adjust the song in the array 'notes' and the length in
// the SLENGTH define.


#include <avr/pgmspace.h>

#define SLENGTH 40

#define OFFS 36

#define nC   0+OFFS
#define nDb  1+OFFS
#define nD   2+OFFS
#define nEb  3+OFFS
#define nE   4+OFFS
#define nF   5+OFFS
#define nGb  6+OFFS
#define nG   7+OFFS
#define nAb  8+OFFS
#define nA   9+OFFS
#define nBb 10+OFFS
#define nB  11+OFFS

const unsigned int freq[] PROGMEM = { \
	 0,0,0,0,0,0,0,0,0,0,0,0, \
	 274,  291,  308,  326,  346,  366,  388,  411,  435,  461,  489,
	 581,  549,  581,  616,  652,  691,  732,  776,  822,  871,  923,
	 976,  1036, 1097, 1163, 1232, 1305, 1383, 1465, 1552, 1644, 1742,
	 1845, 1955, 2071, 2195, 2325, 2463, 2610, 2765, 2930, 3104, 3288,
	 3484, 3691, 3910, 4143, 4389, 4650, 4927, 5220, 5530, 5859, 6207,
	 6577, 6968, 7382, 7821, 8286, 8779, 9301, 9854,10440,11060,11718,
	12415,13153,13935,14764,15642,16572,17557,18601,19709,20897,22121,
	23436,24830,26306,27871,29528,31234,33144,35115,37203,39415,39415,
	41759,44242,46873,49660,52613,55741,59056,62567 };


const unsigned char notes[] PROGMEM = { \
        nC, nD, nEb, nF, nG, nAb, nG, nF, nEb, nD,
        nEb, nF, nEb, nD, nC, nC+12, nB, nC+12, nAb, nG,
        nF, nC+12, nB, nC+12, nG, nF, nEb, nC+12, nB, nC+12,
        nF, nG, nAb, nD, nG, nF, nEb, 0,0,0};

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
  SIDdelay();

  PORTB = (data & 0x0F) | 0x10;  // Lower bit, keep CS deactive
  PORTD = (data & 0xF0) >> 2;    // Upper bit
  PORTB &= ~(0x10);   // Activate /CS
  SIDdelay();
  PORTB |= 0x10;      // Deactivate /CS
  SIDdelay();
}  
  
void setup () {
  char c;     // For SID clearing loop
  
  // First, intialise the ports.
  PORTD = 0;
  PORTB = 0x10;  // PB4 is high to disable Chip Select
  DDRD = 0xFE;   // Keep RxD as an input, everything else output
  DDRB = 0xFF;
  
  // Initialise Timer 0 OC0A to 1MHz on Pin 6.
  TIMSK0 = 0;
  TCNT0 = 0;     // Reset timer
  OCR0A = 0;     // Invert clock after this many clock cycles + 1
  TCCR0A = 0x42; // Initialise Timer 1
  TCCR0B = 0x02; // Prescaler

  // Clear the SID
  for(c=0; c<25;c++)
    wrSID(c,0);

  // Write some default values to the SID
  wrSID(24,0x1F);    // Turn up the volume
  wrSID(5,0x09);      // Fast Attack, Decay
  wrSID(6,0x0A);   // Full volume on sustain, quick release
  wrSID(2,2048&0xFF);
  wrSID(3,2048>>8);
  wrSID(22,192);    // Filter
  wrSID(23,0xF1);
}

void loop () {
  unsigned int n,f, i,d,e;
  for(i=0;i<SLENGTH;i++) {
    wrSID(4,0x20);   // Dosable gate, square waveform.
    n = (unsigned char)pgm_read_word(&notes[i]);
    if (n != 0) {
      f = pgm_read_word(&freq[n]);
      wrSID(0, f);
      wrSID(1, f>>8);
      wrSID(4,0x21);
    }
    for(d=0;d<65000;d++) SIDdelay();
  }
}
