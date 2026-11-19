# MOS 6581 SID — Belles Ondes 资料页（datasheet OCR + 实测经验）

> 来源: <https://www.bellesondes.fr/wiki/doku.php?id=mos6581>  
> 页面日期: 2018-08-26  抓取日期: 2026-11-19  
> 原页面标题: mos6581 [Belles Ondes : Creative Engineering]

![cnmos.jpg cnmos.jpg](https://www.bellesondes.fr/wiki/lib/exe/fetch.php?media=cnmos.jpg) 
**UNDER CONSTRUCTION - FOR INFORMATION ONLY**
## Foreword

This Datasheet is intended to recall the main characteristics of the chip. It combines :
- Data extracted from 'brute' OCR on the scanned MOS datasheets.
- quotes from various Websites
- personnal SID experience/findings.
All the sources are listed in the last section of the Datasheet. It covers all chip versions from 6581 to 8580, the relevant information for each version are precised in the associated chapter when necessary.
### Once upon a time...

In 1982, a young engineer named Bob Yannes was given the task of developing a soundchip for the Commodore company within a four month time frame. 
At that time, he was working for MOS Technology, one of Commodore's main suppliers. This chip was intended to be incorporated in Commodore's new mainstream computer devices. After 4 month of work, the “MOS 6581 Sound Interface Device” - the SID-chip - was born. It proved to be a very well-constructed sound-chip for its time. The SID's possibilities were soon recognised, mainly by the game-producers who assured the huge success of the Commodore 64. Until then there had not been a soundchip of that calibre in a home-computer. The SID remained one of the main chips in the field for a good few years. This chip was Bob Yannes first shot at developing a phase-accumulating oscillator, the fundament of all wavetable synthesis-based systems. At the beginning, he had planned to sell the SID mainly to the synthesizer industry, but the production capacity could hardly keep up with the growing demand for the C64. Once there were enough chips, Yannes wasn't working for Commodore anymore.
Because of its hectic phase of development, full documentation of the chip's possibilities was never finalized: the first one was written for the prototype and it changed so many times that, like Bob Yannes told in an interview with Andreas Varga, it didn't make sense at all anymore. 
### Until the SID enthusiasts took the flag…

## MOS6581 SOUND INTERFACE DEVICE (SID)

The 6581 Sound Interface Device (SID) is a single-chip, 3 voice electronic music synthesizer/sound effects generator compatible with the 65XX and similar microprocessor families. SID provides wide-range, high-resolution control of pitch (frequency), tone color (harmonic content) and dynamics (volume). Specialized control circuitry minimizes software overhead, facilitating use in arcade/home video games and low-cost musical instruments.
## Features

![](https://www.bellesondes.fr/wiki/lib/exe/fetch.php?media=mos6581.png) 
- 3 Tone Oscillators, range 0-4 kHz
- 4 Waveforms per Oscillator
  - Triangle
  - Sawtooth
  - Variable Pulse
  - Noise
- 3 Amplitude Modulators, range 48 dB
- 3 Envelope Generators
- Exponential response
  - Attack Rate: 2mS-8S
  - Decay Rate: 6mS-24S
  - Sustain level: 0-peak volume
  - Release Rate: 6mS-24S
- Oscillator Synchronization
- Ring Modulation
- Programmable Filter
  - Cutoff range: 30 Hz-12 kHz
  - 12 dB/octave Rolloff
  - Low pass, Band pass, High pass, Notch outputs
  - Variable Resonance
- Master Volume Control
- 2 A/D POT Interfaces
- Random Number/Modulation Generator
- External Audio Input
## SID internal Diagram

![sid_diagram.jpg sid_diagram.jpg](https://www.bellesondes.fr/wiki/lib/exe/fetch.php?w=640&h=640&tok=6906c9&media=sid_diagram.jpg) 
## Design Details

- The 3 tone generation Voices are based on 8bit resolution DDS, with 24 bits phase accumulators.
- 4 Waveforms are proposed and digitally combined before reaching a multiplier DAC for amplitude modulation.
- Amplitude modulation is available for each Voice through a configurable ADSR enveloppe generator, and can be triggered separately. ADSR timings are hard coded into Lookup Tables.
- Filter is a Multimode analog VCF filter, with FET operated as VCRs commanded through a 11 bits DAC.
- Output stage is a 4 bit Multiplier DAC, technically placed before the Filter.
## SID Control registers

There are 29 eight-bit registers in SID which control the generation of sound. These registers are either WRITE-only or READ-only and are listed below:

| Zone    | Register | A4  | A3  | A2  | A1  | A0  | Hex Address | D7   | D6   | D5   | D4   | D3   | D2   | D1   | D0   | Name                 | Mode |
| ------- | -------- | --- | --- | --- | --- | --- | ----------- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | -------------------- | ---- |
| Voice 1 | 0        | 0   | 0   | 0   | 0   | 0   | 00          | F7   | F6   | F5   | F1   | F3   | F2   | F1   | F0   | Freq1 Low            | W    |
|         | 1        | 0   | 0   | 0   | 0   | 1   | 01          | F15  | F14  | F13  | F12  | F11  | F10  | F9   | F8   | Freq1 High           | W    |
|         | 2        | 0   | 0   | 0   | 1   | 0   | 02          | PW7  | PW6  | PW5  | PW4  | PW3  | PW2  | PW1  | PW0  | PW1 Low              | W    |
|         | 3        | 0   | 0   | 0   | 1   | 1   | 03          | -    | -    | -    | -    | PW11 | PW10 | PW9  | PW8  | PW1 High             | W    |
|         | 4        | 0   | 0   | 1   | 0   | 0   | 04          | RAND | TRI  | SAW  | SQR  | TEST | RING | SYNC | GATE | Control 1            | W    |
|         | 5        | 0   | 0   | 1   | 0   | 1   | 05          | ATK3 | ATK2 | ATK1 | ATK0 | DEC3 | DEC2 | DEC1 | DEC0 | Attack1 / Decay1     | W    |
|         | 6        | 0   | 0   | 1   | 1   | 0   | 06          | SUS3 | SUS2 | SUS1 | SUS0 | REL3 | REL2 | REL1 | REL0 | Sustain1 / Release1  | W    |
| Voice 2 | 7        | 0   | 0   | 1   | 1   | 1   | 07          | F7   | F6   | F5   | F4   | F3   | F2   | F1   | F0   | Freq2 Low            | W    |
|         | 8        | 0   | 1   | 0   | 0   | 0   | 08          | F15  | F14  | F13  | F12  | F11  | F10  | F9   | F8   | Freq2 High           | W    |
|         | 9        | 0   | 1   | 0   | 0   | 1   | 09          | PW7  | PW6  | PW5  | PW4  | PW3  | PW2  | PW1  | PW0  | PW2 Low              | W    |
|         | 10       | 0   | 1   | 0   | 1   | 0   | 0A          | -    | -    | -    | -    | PW11 | PW10 | PW9  | PW8  | PW2 High             | W    |
|         | 11       | 0   | 1   | 0   | 1   | 1   | 0B          | RAND | TRI  | SAW  | SQR  | TEST | RING | SYNC | GATE | Control 2            | W    |
|         | 12       | 0   | 1   | 1   | 0   | 0   | 0C          | ATK3 | ATK2 | ATK1 | ATK0 | DEC3 | DEC2 | DEC1 | DEC0 | Attack2 / Decay2     | W    |
|         | 13       | 0   | 1   | 1   | 0   | 1   | 0D          | SUS3 | SUS2 | SUS1 | SUS0 | REL3 | REL2 | REL1 | REL0 | Sustain2 / Release2  | W    |
| Voice 3 | 14       | 0   | 1   | 1   | 1   | 0   | 0E          | F7   | F6   | F5   | F4   | F3   | F2   | F1   | F0   | Freq3 Low            | W    |
|         | 15       | 0   | 1   | 1   | 1   | 1   | 0F          | F15  | F14  | F13  | F12  | F11  | F10  | F9   | F8   | Freq3 High           | W    |
|         | 16       | 1   | 0   | 0   | 0   | 0   | 10          | PW7  | PW6  | PW5  | PW4  | PW3  | PW2  | PW1  | PW0  | PW3 Low              | W    |
|         | 17       | 1   | 0   | 0   | 0   | 1   | 11          | -    | -    | -    | -    | PW11 | PW10 | PW9  | PW8  | PW3 High             | W    |
|         | 18       | 1   | 0   | 0   | 1   | 0   | 12          | RAND | TRI  | SAW  | SQR  | TEST | RING | SYNC | GATE | Control 3            | W    |
|         | 19       | 1   | 0   | 0   | 1   | 1   | 13          | ATK3 | ATK2 | ATK1 | ATK0 | DEC3 | DEC2 | DEC1 | DEC0 | Attack3 / Decay3     | W    |
|         | 20       | 1   | 0   | 1   | 0   | 0   | 14          | SUS3 | SUS2 | SUS1 | SUS0 | REL3 | REL2 | REL1 | REL0 | Sustain3 / Release3  | W    |
| Filter  | 21       | 1   | 0   | 1   | 0   | 1   | 15          | -    | -    | -    | -    | -    | FC2  | FC1  | FC0  | Filter Cut Freq Low  | W    |
|         | 22       | 1   | 0   | 1   | 1   | 0   | 16          | FC10 | FC9  | F08  | FC7  | FC6  | F05  | FC4  | F03  | Filter Cut Freq High | W    |
|         | 23       | 1   | 0   | 1   | 1   | 1   | 17          | RES3 | RES2 | RES1 | RES0 | EXT  | FLT3 | FLT2 | FLT1 | Filter Res / Source  | W    |
|         | 24       | 1   | 1   | 0   | 0   | 0   | 18          | 3OFF | HP   | BP   | LP   | VOL3 | VOL2 | VOL1 | VOL0 | Mode                 | W    |
| Inputs  | 25       | 1   | 1   | 0   | 0   | 1   | 19          | PX7  | PX6  | PX5  | PX4  | PX3  | PX2  | PX1  | PXO  | POTX                 | R    |
|         | 26       | 1   | 1   | 0   | 1   | 0   | 1A          | PY7  | PY6  | PY5  | PY4  | PY3  | PY2  | PY1  | PYO  | POTY                 | R    |
|         | 27       | 1   | 1   | 0   | 1   | 1   | 1B          | O7   | O6   | O5   | O4   | O3   | O2   | O1   | O0   | OSC3 / Random Read   | R    |
|         | 28       | 1   | 1   | 1   | 0   | 0   | 1C          | E7   | E6   | E5   | E4   | E3   | E2   | E1   | E0   | ENV3                 | R    |

## SID Registers Description

Comprehensive description of the MOS6581 registers is given herebelow.
### 1 - Freq Low & Hi

| Zone    | Register | A4  | A3  | A2  | A1  | A0  | Hex Address | D7  | D6  | D5  | D4  | D3  | D2  | D1  | D0  | Name       | Mode |
| ------- | -------- | --- | --- | --- | --- | --- | ----------- | --- | --- | --- | --- | --- | --- | --- | --- | ---------- | ---- |
| Voice X | X        | 0   | 0   | 0   | 0   | 0   | 00          | F7  | F6  | F5  | F1  | F3  | F2  | F1  | F0  | FreqX Low  | W    |
|         | X        | 0   | 0   | 0   | 0   | 1   | 01          | F15 | F14 | F13 | F12 | F11 | F10 | F9  | F8  | FreqX High | W    |

Together these registers form a 16-bit number which linearly controls the Frequency of Oscillator 1. The frequency is determined by the following equation:

    Fout = Fn × Fclk / 16777216 Hz

（原页此处为公式图片，未随抓取保存，公式按数据手册文本补全。）

Where Fn is the 16-bit number in the Frequency registers and Fclk is the system clock applied to the Ø2 input (pin 6). For a standard 1.0 Mhz clock, the frequency is given by:

    Fout = (Fn * 0.0596) Hz

A complete table of values for generating 8 octaves of the equally-tempered musical scale with concert A (440 Hz) tuning is provided further. It should be noted that the frequency resolution of SID is sufficient for any tuning scale and allows sweeping from note to note (portamento) with no discernible frequency steps.
Each Voice register can be found at the following address (HEX):

| Voice | Freq Low Address | Freq High Address |
| ----- | ---------------- | ----------------- |
| 1     | 02               | 03                |
| 2     | 09               | 0A                |
| 3     | 10               | 11                |

Note : Those 16 bits are in fact the LSBs of the 24 bit phase accumulator of each Voices.
### 2 - PW Low & Hi

| Zone    | Register | A4  | A3  | A2  | A1  | A0  | Hex Address | D7  | D6  | D5  | D4  | D3   | D2   | D1  | D0  | Name     | Mode |
| ------- | -------- | --- | --- | --- | --- | --- | ----------- | --- | --- | --- | --- | ---- | ---- | --- | --- | -------- | ---- |
| Voice X | X        | 0   | 0   | 0   | 1   | 0   | 02          | PW7 | PW6 | PW5 | PW4 | PW3  | PW2  | PW1 | PW0 | PWX Low  | W    |
|         | X        | 0   | 0   | 0   | 1   | 1   | 03          | -   | -   | -   | -   | PW11 | PW10 | PW9 | PW8 | PWX High | W    |

Together these registers form a 12-bit number (bits 4-7 of PW Hi are not used) which linearly controls the Pulse Width (duty cycle) of the Pulse waveform on Oscillator 1. The pulse width is determined by the following equation:

    PWout = (PWn/40.95) %

Where PWn is the 12-bit number in the Pulse Width registers.
The pulse width resolution allows the width to be smoothly swept with no discernible stepping. Note that the Pulse waveform on Oscillator 1 must be selected in order for the Pulse Width registers to have any audible effect. A value of 0 or 4095 ($FFF) in the Pulse Width registers will produce a constant DC output, while a value of 2048 ($800) will produce a square wave.

| Voice | PW Low Address | PW High Address |
| ----- | -------------- | --------------- |
| 1     | 00             | 01              |
| 2     | 07             | 08              |
| 3     | 0E             | 0F              |

### 3 - Control Register

| Zone    | Register | A4  | A3  | A2  | A1  | A0  | Hex Address | D7   | D6  | D5  | D4  | D3   | D2   | D1   | D0   | Name      | Mode |
| ------- | -------- | --- | --- | --- | --- | --- | ----------- | ---- | --- | --- | --- | ---- | ---- | ---- | ---- | --------- | ---- |
| Voice X | X        | 0   | 0   | 1   | 0   | 0   | 04          | RAND | TRI | SAW | SQR | TEST | RING | SYNC | GATE | Control X | W    |

This register contains eight control bits which select various options on Oscillator 1.
Each Voice register can be found at the following address (HEX):

| Voice | Control Register Address |
| ----- | ------------------------ |
| 1     | 04                       |
| 2     | 0B                       |
| 3     | 12                       |

#### Gate (Bit 0)

The GATE bit controls the Envelope Generator for Voice X. When this bit is set to a one, the Envelope Generator is Gated (triggered) and the ATTACK/DECAY/SUSTAIN cycle is initiated. When the bit is reset to a zero, the RELEASE cycle begins. The Envelope Generator controls the amplitude of Oscillator X appearing at the audio output, therefore, the GATE bit must be set (along with suitable envelope parameters) for the selected output of Oscillator X to be audible. A detailed discussion of the Envelope Generator can be found further.
#### Sync (Bit 1)

The SYNC bit, when set to a one, Synchronizes the fundamental frequency of Oscillator X with the fundamental frequency of Oscillator Y, producing “Hard Sync” effects. Varying the frequency of Oscillator X with respect to Oscillator Y produces a wide range of complex harmonic structures from Voice X at the frequency of Oscillator Y. In order for sync to occur Oscillator Y must be set to some frequency other than zero but preferably lower than the frequency of Oscillator X. No other parameters of Voice Y have any effect on sync.
- Control 1 register syncs VOICE 1 with VOICE3
- Control 2 register syncs VOICE 2 with VOICE1
- Control 3 register syncs VOICE 3 with VOICE2
#### Ring Mod (Bit 2)

The RING MOD bit, when set to a one, replaces the Triangle waveform output of Oscillator X with a “Ring Modulated” combination of Oscillators X and X. Varying the frequency of Oscillator X with respect to Oscillator X produces a wide range of non-harmonic overtone structures for creating bell or gong sounds and for special effects. 
In order for ring modulation to be audible, the Triangle waveform of Oscillator X must be selected and Oscillator Y must be set to some frequency other than zero. No other parameters of Voice Y have any effect on ring modulation. 
- Control 1 register modulates VOICE 1 with VOICE3
- Control 2 register modulates VOICE 2 with VOICE1
- Control 3 register modulates VOICE 3 with VOICE2
#### Test (Bit 3)

The TEST bit, when set to a one, resets and locks Oscillator X at zero until the TEST bit is cleared. The Noise waveform output of Oscillator X is also reset and the Pulse waveform output is held at a DC level. Normally this bit is used for testing purposes, however, it can be used to synchronize Oscillator X to external events, allowing the generation of highly complex waveforms under real-time software control.
#### Triangle Wave (Bit 4)

When set to a one, the Triangle waveform output of Oscillator X is selected. The Triangle waveform is low in harmonics and has a mellow, flute-like quality. The sound of the Triangle wave is 3dB lower than the other sources, the MSB of the internal phase accumulator is used to elaborate the waveform by storing the sign of the wave.
#### Sawtooth Wave (Bit 5)

When set to a one, the Sawtooth waveform of Oscillator X is selected. The sawtooth waveform is rich in even and odd harmonics and has a bright, brassy quality.
#### Square Wave (Bit 6)

When set to a one, the Pulse waveform output of Oscillator X is selected. The harmonic content of this waveform can be adjusted by the Pulse Width registers, producing tone Qualities ranging from a bright, hollow square wave to a nasal, reedy pulse. Sweeping the pulse width in real-time produces a dynamic “phasing” effect which adds a sense of motion to the sound. Rapidly jumping between different pulse widths can produce interesting harmonic sequences.
#### Noise (Bit 7)

When set to a one, the Noise output waveform of Oscillator X is selected. This output is a random signal which changes at the frequency of Oscillator X. The sound quality can be varied from a low rumbling to hissing white noise via the Oscillator X Frequency registers. Noise is useful in creating explosions, gunshots, jet engines, wind, surf and other un-pitched sounds, as well as snare drums and cymbals. Sweeping the Oscillator frequency with Noise selected produces a dramatic rushing effect. One of the output waveforms must be selected for Oscillator X to be audible, however it is NOT necessary to deselect waveforms to silence the output of Voice X. The amplitude of Voice X at the final output is a function of the Envelope Generator only.
NOTE: The oscillator output waveforms are NOT additive. If more than one output waveform is selected simultaneously, the result will be a logical ANDing of the waveforms. Although this technique can be used to generate additional waveforms beyond the four listed above, it must be used with care. If any other waveform is selected while Noise is on, the Noise output can “lock up”. If this occurs, the Noise output will remain silent until reset by the TEST bit or by bringing /RES (pin 5) low.
### 4 - Attack / Decay / Sustain / Release

| Zone    | Register | A4  | A3  | A2  | A1  | A0  | Hex Address | D7   | D6   | D5   | D4   | D3   | D2   | D1   | D0   | Name                | Mode |
| ------- | -------- | --- | --- | --- | --- | --- | ----------- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ------------------- | ---- |
| Voice X | X        | 0   | 0   | 1   | 0   | 1   | 0X          | ATK3 | ATK2 | ATK1 | ATK0 | DEC3 | DEC2 | DEC1 | DEC0 | AttackX / DecayX    | W    |
|         | X        | 0   | 0   | 1   | 1   | 0   | 0X          | SUS3 | SUS2 | SUS1 | SUS0 | REL3 | REL2 | REL1 | REL0 | SustainX / ReleaseX | W    |

Bits 4-7 : ATK0-ATK3 of the first register (ATK0-ATK3) select 1 of 16 ATTACK rates for the Voice X Envelope Generator. The ATTACK rate determines how rapidly the output of Voice 1 rises from zero to peak amplitude when the Envelope Generator is Gated. The 16 ATTACK rates are listed in the table below.
Bits 0-3 : DEC0-DEC3 select 1 of 16 DECAY rates for the Envelope Generator. The DECAY cycle follows the ATTACK cycle and the DECAY rate determines how rapidly the output falls from the peak amplitude to the selected SUSTAIN level. The 16 DECAY rates are listed in the table below.
Bits 4-7 : SUS0-SUS3 of the second register select 1 of 16 SUSTAIN levels for the Envelope Generator. The SUSTAIN cycle follows the DECAY cycle and the output of Voice X will remain at the selected SUSTAIN amplitude as long as the Gate bit remains set. The SUSTAIN levels range from zero to peak amplitude in 16 linear steps, with a SUSTAIN value of 0 selecting zero amplitude and a SUSTAIN value of 15 (#F) selecting the peak amplitude.
A SUSTAIN value of 8 would cause Voice 1 to SUSTAIN at an amplitude one-half the peak amplitude reached by the ATTACK cycle.
Bits 0-3 : RLS0-RLS3 select 1 of 16 RELEASE rates for the Envelope Generator. The RELEASE cycle follows the SUSTAIN cycle when the Gate bit is reset to zero. At this time, the output of Voice 1 will fall from the SUSTAIN amplitude to zero amplitude at the selected RELEASE rate. The 16 RELEASE rates are listed in the table below. 

| DEC | HEX | Attack Rate (Time/Cycle) | Decay & Release Rate (Time/Cycle) |
| --- | --- | ------------------------ | --------------------------------- |
| 0   | 0   | 2 mS                     | 6 mS                              |
| 1   | 1   | 8 mS                     | 24 mS                             |
| 2   | 2   | 16 mS                    | 48 mS                             |
| 3   | 3   | 24 mS                    | 72 mS                             |
| 4   | 4   | 38 mS                    | 114 mS                            |
| 5   | 5   | 56 mS                    | 168 mS                            |
| 6   | 6   | 68 mS                    | 204 mS                            |
| 7   | 7   | 80 mS                    | 240 mS                            |
| 8   | 8   | 100 mS                   | 300 mS                            |
| 9   | 9   | 250 mS                   | 750 mS                            |
| 10  | A   | 500 mS                   | 1.5 S                             |
| 11  | B   | 800 mS                   | 2.4 S                             |
| 12  | C   | 1 S                      | 3 S                               |
| 13  | D   | 3 S                      | 9 S                               |
| 14  | E   | 5 S                      | 15 S                              |
| 15  | F   | 8 S                      | 24 S                              |

Each Voice register can be found at the following address (HEX):

| Voice | ATK / DEC | SUS / REL |
| ----- | --------- | --------- |
| 1     | 05        | 06        |
| 2     | 0C        | 0D        |
| 3     | 13        | 14        |

NOTE: Envelope rates are based on a 1.0 Mhz Ø2 clock. For other Ø2 frequencies, multiply the given rate by 1 Mhz / Ø2. The rates refer to the amount of time per cycle. For example, given an ATTACK value of 2, the ATTACK cycle would take 16 mS to rise from zero to peak amplitude. The DECAY/RELEASE rates refer to the amount of time these cycles would take to fall from peak amplitude to zero.
### 5 - Filter Freq Low & High

| Zone   | Register | A4  | A3  | A2  | A1  | A0  | Hex Address | D7   | D6  | D5  | D4  | D3  | D2  | D1  | D0  | Name                 | Mode |
| ------ | -------- | --- | --- | --- | --- | --- | ----------- | ---- | --- | --- | --- | --- | --- | --- | --- | -------------------- | ---- |
| Filter | 21       | 1   | 0   | 1   | 0   | 1   | 15          | -    | -   | -   | -   | -   | FC2 | FC1 | FC0 | Filter Cut Freq Low  | W    |
|        | 22       | 1   | 0   | 1   | 1   | 0   | 16          | FC10 | FC9 | F08 | FC7 | FC6 | F05 | FC4 | F03 | Filter Cut Freq High | W    |

Together these registers form an 11-bit number (bits 3-7 of FC LO are not used) which linearly controls the Cutoff (or Center) Frequency of the programmable Filter. The approximate Cutoff Frequency ranges between 30Hz and 10KHz with the recommended capacitor values for CAP1 and CAP2. The frequency range of the Filter can be altered to suit specific applications. Refer to the Pin Description section for more information.
### 6 - Filter Res & Signal Source Routing

| Zone   | Register | A4  | A3  | A2  | A1  | A0  | Hex Address | D7   | D6   | D5   | D4   | D3  | D2   | D1   | D0   | Name                | Mode |
| ------ | -------- | --- | --- | --- | --- | --- | ----------- | ---- | ---- | ---- | ---- | --- | ---- | ---- | ---- | ------------------- | ---- |
| Filter | 23       | 1   | 0   | 1   | 1   | 1   | 17          | RES3 | RES2 | RES1 | RES0 | EXT | FLT3 | FLT2 | FLT1 | Filter Res / Source | W    |

Bits 4-7 of this register (RES0-RES3) control the Resonance of the Filler, resonance of a peaking effect which emphasizes frequency components at the Cutoff Frequency of the Filter, causing a sharper sound. There are 16 Resonance settings ranging linearly from no resonance (0) to maximum resonance (15 or #F).
Bits 0-3 of this register indicate whether the output of the Voice is routed to the output, or to the filter.
- EXT : External Input routing
- FLT1 : Voice 1 routing
- FLT2 : Voice 2 routing
- FLT3 : Voice 3 routing

| -      | EXT | FLT3 | FLT2 | FLT1 |
| ------ | --- | ---- | ---- | ---- |
| Output | 0   | 0    | 0    | 0    |
| Filter | 1   | 1    | 1    | 1    |

### 7 - Mode & Volume Control

| Zone   | Register | A4  | A3  | A2  | A1  | A0  | Hex Address | D7   | D6  | D5  | D4  | D3   | D2   | D1   | D0   | Name | Mode |
| ------ | -------- | --- | --- | --- | --- | --- | ----------- | ---- | --- | --- | --- | ---- | ---- | ---- | ---- | ---- | ---- |
| Filter | 24       | 1   | 1   | 0   | 0   | 0   | 18          | 3OFF | HP  | BP  | LP  | VOL3 | VOL2 | VOL1 | VOL0 | Mode | W    |

Bits 4-7 of this register select various Filter mode and output options:
#### LP (Bit 4)

When set to a one, the low Pass output of the Filter is selected and sent to the audio output. For a given Filter input Signal, all frequency components below the Filter Cutoff Frequency are passed unaltered, while all frequency components above the Cutoff are attenuated at a rate of 12 dB/Octave. The low Pass mode produces full-bodied sounds. 
#### BP (Bit 5)

Same as bit 4 for the Band Pass output. All frequency components above and below the Cutoff are attenuated at a rate of 6 dB/Octave. The Band Pass mode produces thin, open sounds. 
#### HP (Bit 6)

Same as bit 4 for the High Pass output. All frequency components above the Cutoff are passed unaltered, while all frequency components below the Cutoff are attenuated at a rate of 12 dB/Octave. The High Pass mode produces tinny, buzzy sounds. 
#### 3 OFF (Bit 7)

When set to a one, the output of Voice 3 is disconnected from the direct audio path. Setting Voice 3 to bypass the Filter (FILT 3 = 0) and selling 3 OFF to a one prevents Voice 3 from reaching the audio output. This allows Voice 3 to be used for modulation purposes without any undesirable output. 
NOTE: The Filter output modes ARE additive and multiple Filter modes may be selected simultaneously. 
For example, both LP and HP modes can be selected to produce a Notch (or Band Reject) Filter response. In order for the Filter to have any audible effect, at least one Filter output must be selected and at least one Voice must be routed through the Filter. The Filter is, perhaps, the most important element in SID as it allows the generation of complex tone colors via subtractive synthesis. The Filter is used to eliminate specific frequency components from a harmonically-rich input signal). The best results are achieved by varying the Cutoff Frequency in real-time.
Bits 0-3 : VOL0-VOL3 select 1 of 16 overall Volume levels for the final composite audio output. The output volume levels range from no output (0) to maximum volume (15 or #F) in 16 linear steps. This control can be used as a static volume control for balancing levels in multi-chip systems or for creating dynamic volume effects, such as Tremolo. Some Volume level other than zero must be selected in order for SID to produce any sound. 
### 8 - Miscelaneous Inputs

#### POTX / POTY

| Zone   | Register | A4  | A3  | A2  | A1  | A0  | Hex Address | D7  | D6  | D5  | D4  | D3  | D2  | D1  | D0  | Name | Mode |
| ------ | -------- | --- | --- | --- | --- | --- | ----------- | --- | --- | --- | --- | --- | --- | --- | --- | ---- | ---- |
| Inputs | 25       | 1   | 1   | 0   | 0   | 1   | 19          | PX7 | PX6 | PX5 | PX4 | PX3 | PX2 | PX1 | PXO | POTX | R    |
|        | 26       | 1   | 1   | 0   | 1   | 0   | 1A          | PY7 | PY6 | PY5 | PY4 | PY3 | PY2 | PY1 | PYO | POTY | R    |

These registers allows the microprocessor to read the position of the potentiometer tied to POTX (pin 24) and POTY (pin25), with values ranging from 0 at minimum resistance, to 255 (#FF) at maximum resistance. The value is always valid and is updated every 512 Ø2 clock cycles. See the Pin Description section for information on pots and capacitor values.
#### OSC 3 / RANDOM

| Zone | Register | A4  | A3  | A2  | A1  | A0  | Hex Address | D7  | D6  | D5  | D4  | D3  | D2  | D1  | D0  | Name               | Mode |
| ---- | -------- | --- | --- | --- | --- | --- | ----------- | --- | --- | --- | --- | --- | --- | --- | --- | ------------------ | ---- |
|      | 27       | 1   | 1   | 0   | 1   | 1   | 1B          | O7  | O6  | O5  | O4  | O3  | O2  | O1  | O0  | OSC3 / Random Read | R    |

This register allows the microprocessor to read the upper 8 output bits of Oscillator 3. The character of the numbers generated is directly related to the waveform selected. If the Sawtooth waveform of Oscillator 3 is selected, this register will present a series of numbers incrementing from 0 to 255 ($FF) at a rate determined by the frequency of Oscillator 3. If the Triangle waveform is selected, the output will increment from 0 up to 255, then decrement down to 0. If the Pulse waveform is selected, the output will jump between 0 and 255. Selecting the Noise waveform will produce a series of random numbers, therefore, this register can be used as a random number generator for games. There are numerous timing and sequencing applications for the OSC 3 register, however, the chief function is probably that of a modulation generator. The numbers generated by this register can be added, via software, to the Oscillator or Filter Frequency registers or the Pulse Width registers in real-time. Many dynamic effects can be generated in this manner. Siren-like sounds can be created by adding the OSC 3 Sawtooth output to the frequency control of another oscillator. Synthesizer “Sample and Hold” effects can be produced by adding the OSC 3 Noise output to the Filter Frequency control registers. Vibrato can be produced by selling Oscillator 3 to a frequency around 7 Hz and adding the OSC 3 Triangle output (with proper scaling) to the Frequency control of another oscillator. An unlimited range of effects are available by altering the frequency of Oscillator 3 and scaling the OSC 3 output. Normally, when Oscillator 3 is used for modulation, the audio output of Voice 3 should be eliminated (3 OFF = 1).
#### ENV 3

| Zone | Register | A4  | A3  | A2  | A1  | A0  | Hex Address | D7  | D6  | D5  | D4  | D3  | D2  | D1  | D0  | Name | Mode |
| ---- | -------- | --- | --- | --- | --- | --- | ----------- | --- | --- | --- | --- | --- | --- | --- | --- | ---- | ---- |
|      | 28       | 1   | 1   | 1   | 0   | 0   | 1C          | E7  | E6  | E5  | E4  | E3  | E2  | E1  | E0  | ENV3 | R    |

Same as OSC 3, but this register allows the microprocessor to read the output of the Voice 3 Envelope Generator. This output can be added to the Filler Frequency to produce harmonic envelopes, WAH WAH, and similar effects. “Phaser” sounds can be created by adding this output to the frequency control registers of an oscillator. The Voice 3 Envelope Generator must be gated in order to produce any output from this register. The OSC 3 register, however, always reflects the changing output of the oscillator and is not affected in any way by the Envelope Generator.
## SID Pinout and Connection

Pins decription can be found herebelow. Consider jumping to next section for maximum ratings.

### Power Supply

SID power consumption during normal operation is around 600mW. Expect a little more from a 6581 (around 650mW), and less from 6582 and 8580 (around 550W). The SID shall heat a lot during operation. The design of the analog part of the chip has been improved from 6581 to 6582/8580, allowing to reduce the operating voltage of the chip, and at the same time lowering its consumed power.
This also means that 6581 and 6582/8580 are not compatible. The power voltages listed below shall be respected, otherwise SID might be damaged.
#### VCC - Pin 25

This pin shall be powered by a 5V DC coupled power supply. About 70mA is drawn from VCC during normal operation. Coupling capacitor of at least 10uf and filtering capacitor of 100nf are recommended for proper operation.
#### VDD - Pin 28

This pin shall be connected to either 9V or 12V DC coupled power supply, according to your SID chip version:

| SID Model | VDD Voltage | Current |
| --------- | ----------- | ------- |
| 6581      | 12V         | 25mA    |
| 6582      | 9V          |         |
| 8580      | 9V          | —       |

Filtering capacitor of 100nf is recommended.
#### GND - Pin 14

Signal Grounds. To lower the noise, the digital and analog ground shall be connected together on this pin and only at it. Keep in mind that the VDD is used only by the analog part of the chip, which includes it inside the analog section. This makes a basic ground plane domain for analog around pins 26, 27 and 28.
### Filter Configuration

#### CAP1A CAP1B CAP2A CAP2B - Pin 1-2 & Pin 3-4

These pins are used to connect the two integrating capacitors required by the internal state variable filter to operate. Both caps shall be identical, and matched if the user intends to mix several MOS together. Since the design has been significantly updated from 6581 to 8580 versions, the basic values for the capacitor changes from one version to the other. The following cap values are given for a nominal 20Hz - 12kHz range:

| Version | Cap value | Max Freq                        |
| ------- | --------- | ------------------------------- |
| 6581    | 1 to 2nF  | 10k, provided Fmax=0.000026/Cap |
| 6582    | 6.8nF     | 12k, provided Fmax=0.000052/Cap |
| 8580    | 6.8nF     | 12k, provided Fmax=0.000052/Cap |

Note: Depending on the 6581 version, early units require 1nF as per 1986 datasheet. However there are few of those units out here.
Increasing the capacitor value shifts the maximum filter output frequency down, allowing more tuning control capability over the 11bit control range of the filter. Basic asumption of the maximum filter frequency is given column 3 of the table above. The filter control range covers 9 octaves below the Fmax frequency
Both capacitors shall be implemented as close as possible to the chip. The signal flowing through them is analog.
### Digital Pins

#### \_RES - Pin 5

This TTL-level input is the reset control for SID. When brought low for at least ten Ø2 cycles, all internal registers are reset to zero and the audio output is silenced. This pin is normally connected to the reset line of the microprocessor or a power-on-clear circuit.
#### Ø2 - Pin 6

This TTL-level input is the master clock for SID. Ø2 controls both system data transfers between SID and the microprocessor and the DDS synth.
System Clock : Data can only be transferred when Ø2 is high. Essentially, Ø2 acts as a high-active chip select as far as data transfers are concerned. This pin is normally connected to the system clock, with a nominal operating frequency of 1.0 MHz. The clock can be cristal based or generated by another component like a microprocessor timer.
DDS Synth : The 1Mhz clock also drives the phase accumulator counter that DDS synth uses. As a consequence, the accuracy of the Ø2 clock frequency is linked to the accurate tuning of the audio outputs, aside from a good configuration of the frequency registers. Hence, great care shall be given to the Ø2 clock source in terms of jitter and skew for accurate tuning. 
#### R/\_W - Pin 7

This TTL-level input controls the direction of data transfers between SID and the microprocessor. If the chip select conditions have been met, a high on this line allows the microprocessor to Read data from the selected SID register and a low allows the microprocessor to Write data into the selected SID register. This pin is normally connected to the system Read/Write line.
#### \_CS - Pin 8

This TTL-level input is a low active Chip select which controls data transfers between SID and the microprocessor. /CS must be low for any transfer.

| Action | _CS State | Ø2 State | R/_W State |
| ------ | --------- | -------- | ---------- |
| Read   | low       | high     | high       |
| Write  | low       | high     | low        |

This pin is normally connected to address decoding circuitry, allowing SID to reside in the memory map of a system.
#### A0-A4 - Pins 9-13

These TTL-level inputs are used to select one of the 29 SID registers. The remaining three register locations are not used. A Write to any of these three locations is ignored and a Read returns invalid data.
These pins are normally connected to the corresponding address lines of the microprocessor so that SID may be addressed in the same manner as memory.
#### D0-D7 - Pins 15-22

These bidirectional lines are used to transfer data between SID and the microprocessor. They are TTL compatible in the output mode and capable of driving 2 TTL loads in the output mode. The data buffers are usually in the high-impedance off state. During a Write operation, the data buffers remain in the off (input) state and the microprocessor supplies data to SID over these lines. During a Read operation, the data buffers turn on and SID supplies data to the microprocessor over these lines. The pins are normally connected to the corresponding data lines of the microprocessor.
### Analog Pins

#### POTX & POTY - Pins 24 & 23

These pins are inputs to the A/D converters used to digitize the position of potentiometers. The conversion process is based on the time constant of a capacitor tied from the POT pin to ground, charged by a potentiometer tied from the POT pin to +5 volts. The component values are determined by RC = 4.7E-4  where R is the maximum resistance of the pot and C is the capacitor. The larger the capacitor, the smaller the POT value jitter. 
The recommended values for R and C are 470 KOhms and 1000 pF.
#### Ext In - Pin 26

This analog input allows external audio signals to be mixed with the audio output of SID or processed through the Filter. Typical sources include voice, guitar and organ. The input impedance of this pin is in the order of 100 KOhms. Any signal applied directly to the pin should ride at DC level of 6 volts and should not exceed 3 volts p-p. In order to prevent any interference caused by DC level differences, external signals should be AC-coupled to EXT IN by an electrolytic capacitor in the 1-10uF range. As the direct audio path (FILTEX = 0) has unity gain, EXT IN can be used to mix outputs of many SID chips by daisy-chaining. The number of chips that can be chained in this manner is determined by the amount of noise and distortion allowable at the final output. Note that the output Volume control will affect not only the three SID voices, but also any external inputs.
#### Audio Out - Pin 27

This open-source buffer is the final audio output of SID, comprised of the three SID voices, the Filter and any external input. The output level is set by the output Volume control and reaches a maximum of approximately 3 volts p-p at a 6 volt DC level. A source resistor from AUDIO OUT to ground is required for proper operation. The recommended resistance is 1 KOhm for a standard output impedance. As the output of SID rides at a 6 volt DC level, it should be AC-coupled to any audio amplifier with an electrolytic capacitor in the 1-10uF range.
