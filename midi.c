#include <avr/io.h>
#include <avr/interrupt.h>

/* FIFO_LEN must be equal to a power of two minus one */
#define FIFO_LEN 63

/* The number of channels available */
#define C_TOTAL 3

/* Voices:			*/
/* 	0 = Triangle		*/
/*	1 = Sawtooth		*/
/*	2 = Square		*/
/*	3 = Noise		*/

#define VOICE 2

/* Pulse width must be less than 0x0FFF (4095) */
#define PULSE_WIDTH 0x500

/* These all must be below 0x0F (15) */
#define ATTACK 6
#define DECAY  0
#define SUSTAIN 15
#define RELEASE 2

/* Enable these filters. If none are enabled */
/* the filter will be turned off.            */
#define LP 1
#define BP 0
#define HP 0

/* Resonance must be below 0x0F (15) */
#define RESONANCE 15

/* Filter Value must be below 0x07FF (2047) */
#define FILTER_STA 500
#define FILTER_END 1500
/* Filter change speed */
#define FILTER_SPE 100

/* Filter Type:			*/
/*	0 = Automatic		*/
/*	1 = Manual (from ADC)	*/
#define FILTER_TYPE 1

/* 1 to enable vibrato, 0 to disable */
#define VIB_ENABLE 1
/* You don't really want this to be above 3 */
#define VIB_STRENGTH 1
/* 8-bit counter value. Keep below 255 */
#define VIB_SPEED 255
/* Wait this many interrupts before enabling vibrato */
#define VIB_DELAY 150

/* Some calculations here */
#define FILTER_CALC (0x10 * LP) | (0x20 * BP) | (0x40 * HP)

char chan_tab [C_TOTAL];	/* Channel usage table */
char fifo[FIFO_LEN + 1];	/* FIFO array used for incoming data */
int fifo_in;			/* FIFO incoming pointer */
int fifo_out;			/* FIFO outgoing pointer */
int filter;			/* Current filter value */
int filter_dir;			/* Filter direction */
int orig_osccal;
char osccal_dir;
int osccal_count;		/* for delay between start of note and vibrato */

int freq[] = { \
	 0,0,0,0,0,0,0,0,0,0,0,0, \
	 274,  291,  308,  326,  346,  366,  388,  411,  435,  461,  489,  \
	 581,  549,  581,  616,  652,  691,  732,  776,  822,  871,  923,  \
	 976,  1036, 1097, 1163, 1232, 1305, 1383, 1465, 1552, 1644, 1742, \
	 1845, 1955, 2071, 2195, 2325, 2463, 2610, 2765, 2930, 3104, 3288, \
	 3484, 3691, 3910, 4143, 4389, 4650, 4927, 5220, 5530, 5859, 6207, \
	 6577, 6968, 7382, 7821, 8286, 8779, 9301, 9854,10440,11060,11718, \
	12415,13153,13935,14764,15642,16572,17557,18601,19709,20897,22121, \
	23436,24830,26306,27871,29528,31234,33144,35115,37203,39415,39415, \
	41759,44242,46873,49660,52613,55741,59056,62567 };

char read_fifo () {
	char c;
	while (fifo_in == fifo_out);
	c = fifo[fifo_out++];
	fifo_out &= FIFO_LEN;
	return c;
}

void write_sid (char reg, char dat) {
	cli();						/* So ISRs don't interfere */

	PORTD = (reg & 0b00000010) | 0b01000000;	/* Lower CLK line on '374 */
	PORTB = (reg & 0b00011101);			/* Put data onto data bus */
	PORTD = (reg & 0b00000010) | 0b11000000;	/* Raise CLK line on '374 */
	
	PORTB = dat & 0b11111101;			/* Put data onto data bus */
	PORTD = (dat & 0b00000010) | 0b01000000;

	PORTD = dat & 0b00000010;			/* Lower /CS */
	PORTD = (dat & 0b00000010) | 0b01000000;	/* Raise /CS */

	sei();
}	

ISR (USART_RXC_vect) { 
	fifo[fifo_in++] = UDR;
	fifo_in &= FIFO_LEN;
}

ISR (TIMER2_COMP_vect) {
	int i;
	#if VIB_ENABLE
	if (osccal_count >= VIB_DELAY) {
		i = OSCCAL;
		if (i < orig_osccal - VIB_STRENGTH) osccal_dir = 0;
		if (i > orig_osccal + VIB_STRENGTH) osccal_dir = 1;
		if (osccal_dir) OSCCAL = i - 1;
		else OSCCAL = i + 1;
	} else {
		OSCCAL = orig_osccal;
		osccal_count += 10;
	}
	#endif

	#if FILTER_TYPE
	if (filter != ADCH) {
		filter = ADCH;
		write_sid (0x16, filter);
	}
	#else
	if (filter < FILTER_END) {
		filter += FILTER_SPE;
		write_sid (0x16, (filter>>3));
	}
	/* if (filter > FILTER_END) filter_dir = 0;
	if (filter < FILTER_STA) filter_dir = 1;
	if (filter_dir) filter += FILTER_SPE;
	else filter -= FILTER_SPE;
	write_sid (0x16, (filter>>3)); */
	#endif
}
	

void strike_note (char n) {
	int chan, c, f;

	chan = 0;
	c = 0;
	for(;;) {
		if (c == C_TOTAL) return;	/* Table is full */
		/* found an empty channel */
		if (chan_tab[c] == (char)0xFF) break;
		chan += 7;	/* Increment to next channel in the SID*/
		c++;
	}		

	chan_tab[c] = n;			/* Record where note is on */
	f = freq[n];
	
	write_sid (chan, (char)f);
	write_sid (chan + 1, (char)(f >> 8));
	write_sid (chan + 4, (char)(1<<(VOICE+4))|1);		/* Voice on */
}

void release_note (char n) {
	int chan, c;
	chan = 0;
	c = 0;

	for (;;) {
		if (c == C_TOTAL) return;	/* Note not found */
		if (chan_tab[c] == n) break;
		chan += 7;			/* Increment to next channel in the SID*/
		c++;
	}

	chan_tab[c] = (char)0xFF;		/* Record that channel is off */
	write_sid (chan + 4, (char)1<<(VOICE+4));
}

void init() {

	int i;

	OSCCAL -= 7;		/* Calibrate the internal oscillator */
				/* You should read 1MHz on pin 15 */
	orig_osccal = OSCCAL;

	PORTD = 0b01000000;	/* /CS high */
	PORTB = 0b00000000;

	DDRB = 0b11111111;	/* PORTB All Outputs */
	DDRC = 0b00000000;	/* PORTC All Inputs */
	DDRD = 0b11111110;	/* PORTD outputs except RXD */

	/* Initialize Timer1 for a 1MHz output */
	TCNT0 = 0;		/* Reset timer, just in case */
	OCR1A = 3;		/* Invert output after this many clock cycles plus one*/
	TCCR1B = 0b00001001;
	TCCR1A = 0b01000000;

	/* Initialize FIFO pointers */
	fifo_in = 0;
	fifo_out = 0;

	/* Initialize USART for MIDI */
	UBRRH = 0;		/* 31250bps @ 8MHz */
	UBRRL = 15;
	UCSRC = 0b10000110;	/* Select UCSRC, 8 bits, no parity */
	UCSRB = 0b10010000;	/* Receive enable, Receive IRQ enable */

	/* Initialize Timer 2 for approx 50Hz, used for filter */
	TCCR2 = 0b01000110;	/* CTC mode, prescaler of 1024 */
	OCR2 = VIB_SPEED;
	TIMSK = 0b10000000;	/* Interrupt Enabled for Timer 2 */

	/* Initialize ADC */
	ADMUX = 0b00100000;	/* Channel 0, ADLAR set to read easily */
	ADCSRA = 0b11100000;	/* Free running */

	/* Initialize SID */
	for (i = 0; i < 22; i += 7) {
		write_sid ((char)(i + 0x05), ((ATTACK<<4)|DECAY));	/* Attack / Decay */
		write_sid ((char)(i + 0x06), ((SUSTAIN<<4)|RELEASE));	/* Sustain / Release */
		write_sid ((char)(i + 0x02), (PULSE_WIDTH & 0xFF));	/* Pulse Width low */
		write_sid ((char)(i + 0x03), (PULSE_WIDTH >> 8));	/* Pulse Width high */
		write_sid ((char)(i + 0x04), (1<<(VOICE+4)));		/* Voice disabled */
	}
	write_sid (0x16, FILTER_STA >> 3);
	
	filter_dir = 0;

	#if LP | BP | HP
		write_sid (0x17, (RESONANCE << 4) | 0x07);
	#endif

	write_sid (0x18, (FILTER_CALC | 0x0F));

	/* Initialize tables */
	for (i = 0; i < C_TOTAL; i++) {
		chan_tab[i] = (char)0xFF;
	}
	
	sei();
}

int main() {
	char data, l, note;

	init();

	data = read_fifo();	/* Start off with something in data */
	for(;;) {
		l = 1;
		switch (data) {
			case ((char)0x80):
				for(;;) {
					l = 0;
					data = read_fifo();
					if (data & (char)0x80) break;

					release_note(data);

					data = read_fifo();
					if (data & (char)0x80) break;
				}
				break;

			case ((char)0x90):
				for(;;) {
					l = 0;
					data = read_fifo();		/* get a note */
					if (data & (char)0x80) break;	/* If not a note, interpret commands */

					note = data;
					filter = FILTER_STA;
					osccal_count = 0;

					data = read_fifo();		/* Get the velocity. If zero, strike */
					if (data & (char)0x80) break;	/* is actually release. */

					if (data == 0) release_note(note);
					else strike_note(note);
				}
				break;
		}	
		if (l) data = read_fifo();			/* If we reach here with nothing, read FIFO again */
	}
}
