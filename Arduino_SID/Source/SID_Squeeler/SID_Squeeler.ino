// Written 20-Dec-2011
// Alexis Kotlowy
//
// Released into the Public Domain
//
// Just a simple noise generator to test the SID.

#include <avr/pgmspace.h>

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
  OCR0A = 3;     // Invert clock after this many clock cycles + 1
  TCCR0A = 0x42; // Initialise Timer 1
  TCCR0B = 0x02;

  // Clear the SID
  for(c=0; c<25;c++)
    wrSID(c,0);

  // Write some default values to the SID
  wrSID(24,15);    // Turn up the volume
  wrSID(5,0);      // Fast Attack, Decay
  wrSID(6,0xF0);   // Full volume on sustain, quick release
  wrSID(4,0x21);   // Enable gate, sawtooth waveform.

}

void loop () {
  unsigned int i;
  for(;;) {
    wrSID(0, i);
    wrSID(1, i>>8);
    i++;
  }

}
