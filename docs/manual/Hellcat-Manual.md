# Hellcat User Manual

<!--pagebreak-->

## 1. What Hellcat Is

Hellcat is a software synthesizer. It re-creates the Mutable Instruments Ambika.
Ambika is a hybrid polysynth from 2012. Mutable Instruments released its firmware under an open license. Hellcat runs that engine on a modern computer.

The Ambika design splits one voice into two parts:

- The digital part generates the oscillators, the mix, the envelopes and the modulation.
- The analog part holds the resonant filter and the final amplifier.

Hellcat keeps this split. The digital engine is a faithful port. The analog filter cards run as circuit models.

### Parts and voicecards

The hardware Ambika holds six voicecards. One voicecard is one complete voice.
A controller board splits incoming notes between the cards.

Hellcat follows the same plan:

- The instrument has six parts. Each part holds its own patch.
- Each part takes MIDI on its own channel and key zone.
- Each part takes voices from a shared pool. The pool holds 96 voices.
- A part can use 1 to 16 voices. Set the count on the Patch page.

### The sound of one voice

One voice runs through these blocks, in order:

1. Two oscillators with many waveform algorithms.
2. A mixer with special two-oscillator operations, a sub oscillator and noise.
3. A resonant filter card. You select the card type.
4. A final amplifier with two response curves.

<!--pagebreak-->

## 2. Install and Formats

### Formats

| Format | Use |
|---|---|
| Standalone | Runs alone. No host program needed. |
| VST3 | Loads into most DAW software. |
| AU | Loads into Logic Pro, GarageBand and AU hosts. |
| CLAP | Loads into CLAP hosts, for example Bitwig and Reaper. |

### System needs

- macOS 13 or newer. Apple Silicon hardware.
- A 64-bit host for the plugin formats.
- An iPad build exists for iOS 14 and newer. It is not on the App Store yet.

### Install

Copy the bundles into these folders. Then restart your host and rescan plugins.

| Format | Install folder |
|---|---|
| VST3 | ~/Library/Audio/Plug-Ins/VST3 |
| AU | ~/Library/Audio/Plug-Ins/Components |
| CLAP | ~/Library/Audio/Plug-Ins/CLAP |
| Standalone | /Applications |

macOS may quarantine bundles you copy by hand. Clear the flag once with this command:

`xattr -cr ~/Library/Audio/Plug-Ins/Components/Hellcat.component`

Signed release builds do not show this prompt.

<!--pagebreak-->

## 3. Quick Start

This sequence makes a sound in about one minute.

1. Open Hellcat in your host. Or start the Standalone app.
2. Play a note from your MIDI keyboard.
3. The factory patch plays. Both oscillators run.
4. Open the Synth page if it is not open.
5. Find the oscillator knobs. Turn "Osc 1 Shape" and pick another waveform.
6. Find the filter knobs. Turn "Filter 1 Cutoff" and listen.
7. Raise "Filter 1 Resonance" slowly. The filter starts to ring.
8. Press the Patch page button. Select a template from the Arrangement list.
9. Play again. Each template changes the whole instrument.
10. Press the FX page button. Turn the first slot "FX1 Enable" on.
11. Pick an effect. Turn "FX1 Dry/Wet" up halfway.

No sound from the Standalone app? Open its audio settings. Select your audio device and an output pair.

No sound in a host? Check these items:

- The track record monitor is on.
- The track output routes to the master bus.
- MIDI reaches the track. Play a note and watch the MIDI indicator.

### Play without a MIDI keyboard

The Standalone app accepts computer keys. The key row A to L plays notes like a piano.
The keys W, E, T and Y raise sharps. Keys Z and X shift the octave.
Press the KBD button to show the on-screen keyboard strip. Click its keys with the mouse.

<!--pagebreak-->

## 4. The Interface Tour

The window holds three main pages. Buttons in the header switch pages.

### Synth page

The Synth page shows the sound controls of the current part.

![The Synth page in the default Carbon theme. The top row holds the Oscillator, Mixer and Filter panels. The bar under it shows the modulation pills. The bottom row holds the active editor and the mod matrix.](screens/01_Synth_overview.png)

- Oscillator section: shape, parameter, range and detune for two oscillators.
- Mixer section: balance, operation, sub oscillator, noise, fuzz and crush.
- Filter section: cutoff, resonance, mode, filter envelope and filter LFO amounts.
- Envelope and LFO block: three envelope and LFO pairs. Each pair shares one slot.
- Mod matrix: fourteen rows. Each row routes a source to a destination.
- Modifier section: four modifiers. Each modifier computes a new modulation value.

### FX page

The FX page shows the effect chain of the current part.

![The FX page in the default Carbon theme. Three effect slot cards sit at the left. The shared generator editor and the FX matrix sit at the bottom.](screens/16_FX_overview.png)

- Three effect slot cards. Each card holds one effect with five parameters.
- A topology selector. It sets how the three slots connect.
- The FX mod matrix. It routes modulation into effect parameters.
- A master section with wet/dry mix and a three-band EQ.

### Patch page

The Patch page manages parts and voices.

- A grid of six parts. Each part row shows its name, channel and zone.
- Voice slot counts for every part. The counts sum against the 96-voice pool.
- The Tune column. It sets the scale preset, tuning and spread per part.
- An arrangement selector. It loads complete part setups with one action.

### The mod bar and telemetry

A bar under the pages shows live modulation. Each source gets a pill.
A pill draws a short history trace of its value. Traces move while you play.
The bar also shows which modulation rows act at this moment. This helps you debug a patch.

![A generator selected in the shared editor. The LFO 1 pill is active in the mod bar. The bottom row shows the LFO editor and the mod matrix.](screens/04_LFO_1.png)

### Tooltips

Every control carries a tooltip. Rest the mouse pointer on a control.
The tooltip gives one sentence about the control. The appendix lists every tooltip.

<!--pagebreak-->

## 5. Synthesis Architecture

### Oscillators

Each voice holds two oscillators. Each oscillator picks one algorithm:

![The Oscillator panel of the Synth page. Shape selector, Param knob, Range and Detune for both oscillators.](screens/33_Oscillators.png)

- Classic shapes: saw, square, triangle, sine.
- CZ phase-distortion shapes in several flavors.
- FM with a ratio parameter.
- Wavetables one to sixteen. The parameter scans the table.
- Wavequence, a wavetable sequencer.
- Special shapes: filtered noise, vowel, dirty PWM, 8-bit land.

The "Param" knob changes meaning with the shape. It sets pulse width, FM ratio, wavetable position and more.

### Range and detune

"Range" shifts an oscillator by octaves. The span is plus or minus 24 semitones.
"Detune" fine-tunes an oscillator. The span is about plus or minus half a semitone.
Detune the two oscillators slightly for a wide ensemble tone.

### The mixer

The mixer combines both oscillators and adds extra sources.

![The Mixer panel. Balance, operation, sub oscillator, noise, fuzz and crush.](screens/34_Mixer.png)

- "Balance" crossfades between oscillator one and two.
- "Mix Op" selects the two-oscillator operation.
- "Sub Shape" and "Sub Level" add a sub oscillator one octave down.
- "Noise" adds white noise.
- "Fuzz" adds wavefolding distortion.
- "Crush" lowers the bit depth. It emulates the hardware sample-and-hold.

| Mix Op | Effect |
|---|---|
| Sum | Adds both oscillators. |
| Sync | Oscillator 2 resets oscillator 1. Classic sync tone. |
| Ring Mod | Multiplies both oscillator signals. Metallic tones. |
| XOR | Combines both square waves by logic. Chip-tune texture. |
| Fold | Wavefolds the sum. Adds harmonics. |
| Bits | Masks bits off the mix. Crunchy digital texture. |

<!--pagebreak-->

### The filter cards

The filter is the heart of the Ambika sound. Hellcat models six analog filter boards.
Select a card on the Global options. The choice applies to every part. One hardware unit holds one card type, so the program follows this rule.

| Card | Poles | Character |
|---|---|---|
| SMR4 | 4 | The stock Ambika card. OTA cascade with a warm knee. Self-oscillates. |
| 4P | 4 | A clean cascade. Linear and smooth. |
| SVF | 2 | State-variable filter. Gives LP, BP, HP and notch modes. |
| Ladder | 4 | Transistor ladder model. Rich saturation. Self-oscillates at full resonance. |
| Polivoks | 2 | Soviet-era design. Gritty growl at high drive. |
| IR3109 | 4 | The Juno-class cascade. Polite stages. Never self-oscillates. |

SMR4 is the default card. Every Ambika unit shipped with this board.
All cards play at matched loudness. The program keeps card levels close at equal settings.

![The Filter panel. Cutoff and resonance knobs, the mode selector and the frequency response preview. The preview draws the curve of the selected card.](screens/35_Filter.png)

The four-pole cards are lowpass only. The hardware boards have one lowpass output.
The SVF and Polivoks cards honor the mode knob. Polivoks offers LP and BP.
Mode HP and notch clamp to LP on Polivoks.

"Filter Drive" sets the filter input drive. It acts on Ladder, SMR4, IR3109 and Polivoks.
The default value 1.2 matches the hardware calibration. Higher values clip lower.

### Filter tracking and pre-routes

The cutoff tracks the played note. Higher notes open the filter. This is key tracking.
Two pre-routes always act, like on the hardware:

- Envelope 2 scales the cutoff by the "Filter Env" amount.
- LFO 2 scales the cutoff by the "Filter LFO" amount.

Mod matrix rows stack on top of these pre-routes.

### Envelopes and LFOs

Each part holds three envelope and LFO pairs. One pair shares one slot, as on the hardware.

![The Envelopes page. Attack, decay, sustain and release for all three envelopes.](screens/36_Envelopes.png)

- Envelopes: attack, decay, sustain, release. Envelope 1 drives the amplifier by default.
- LFOs: triangle, square, sample-and-hold, ramp. Rate values under 15 lock to host tempo.
- Each LFO picks a sync mode. "Free" runs on its own clock. "Slave" resets on new notes.

A fourth voice LFO runs inside each voice. It restarts with every note.

### The final amplifier

A VCA closes each voice. Two response curves exist:

- Linearized: the output tracks the envelope in a straight line. The default.
- Exponential: the output follows an analog-style curve. Faster perceived decay.

Select the curve in the Global options.

<!--pagebreak-->

## 6. Modulation, Modifiers, Sequencer and Arp

### The mod matrix

The matrix holds fourteen rows. Each row has three parts:

1. A source. Pick from envelopes, LFOs, sequencers, velocity, notes and more.
2. A destination. Pick from over nineteen targets.
3. An amount from -63 to +63. Negative inverts the source.

The last row scales with the modulation wheel. The hardware works the same way.
Some destinations act in special ways:

- "Filter Cutoff" adds to the key tracking and the two filter pre-routes.
- "VCA" multiplies the amplifier level. It does not add.
- "Osc 1+2 Coarse" and "Fine" shift the pitch of both oscillators.

### Modifiers

A modifier computes one new modulation value from two sources. Four modifiers exist.
Pick the operation per modifier:

![The Modifiers page. Four modifier rows, each with two operand selectors and one operation selector.](screens/38_Modifiers.png)

| Operation | Result |
|---|---|
| Sum | Adds both inputs. |
| Product | Multiplies both inputs. |
| Attenuate | Scales one input by the other. |
| Max / Min | Picks the larger or smaller input. |
| XOR | Combines both inputs by logic. |
| GE / LE | Outputs full on when one input passes the other. |
| Quantize | Masks low bits off the input. Stepped values. |
| Lag | Smooths the input over time. |

Modifiers feed the matrix as sources "Op 1" to "Op 4".

### Step sequencers

Three sequencers run per part:

![The Sequencer page with the step grid and the length control.](screens/39_Sequencer.png)

- Sequencer 1 and 2 output modulation values. Each holds up to 16 steps.
- The note sequencer plays notes with gate and velocity per step.

Set a length for each sequencer. Steps beyond the length stay silent.
The sequencers sync to host tempo. Use them as modulation sources in the matrix.

### The arpeggiator

The arp engine mode lives in "Arp Mode": Off, Arp or Sequencer.

![The Arp page. Direction, octave span, pattern and resolution controls.](screens/40_Arp.png)

- "Arp Direction" sets the note order. Up, down, up-down, as-played, random or chord.
- "Arp Octave" spans one to four octaves.
- "Arp Pattern" gates notes from a set of 22 stored patterns.
- "Arp Resolution" sets the rhythmic value, from whole notes to 1/64 triplets.

The arp follows the part scale. Octave shifts move by one scale period.

<!--pagebreak-->

## 7. Effects

Each part runs its own effect chain. Three slots hold one effect each.

![The FX page with an active Reverb in slot 1. The wet/dry mix routes to envelope 1 through the FX matrix.](screens/32_FX_reverb_active.png)

### Chain topologies

| Topology | Signal path |
|---|---|
| FX1 -> FX2 -> FX3 | One series chain through all slots. |
| FX1 + FX2 -> FX3 | Slots 1 and 2 run parallel. Both feed slot 3. |
| FX1 -> FX2 + FX3 | Slot 1 feeds slots 2 and 3 in parallel. |

### Effect types

The slots offer 25 effects plus a None entry. The effects form three families.

The first family ports Mutable Instruments Clouds-class code:

- Diffuser, a granular cloud engine.
- Pitch Shifter and Pitch Stretch.
- CVerb, Spectral and Looping Delay.
- Wavefolder, Frequency Shifter, Ring Modulator, Resonator.

The second family emulates the Spin FV-1 DSP chip. It runs at a fixed 32 kHz rate and models the chip limits:

- Clocked Delay, Ensemble, Plate.
- Vinyl Compressor, Phaser.
- Overdrive and Wavemangler.

A third set covers studio staples: Chorus, Flanger, Digital Echo, Room, Spring, Dual-BBD Chorus, Compressor and Gate.

Each slot exposes five parameters. The slot card shows each parameter name for the selected effect.

### The FX mod matrix

Sixteen rows route modulation into effect parameters. Sources match the synth matrix.
Targets cover each slot dry/wet and all five parameters per slot.

### The master section

A master stage follows the chain:

- "FX Mix" blends the whole chain against the dry signal.
- "FX EQ Low" cuts low rumble. Zero disables the cut.
- "FX EQ Mid" and "FX EQ High" trim bands. Value 64 is unity.

<!--pagebreak-->

## 8. Presets and File Formats

### Preset storage

The plugin bundles factory banks. Browse them from the Patch page.
User presets save into your own folder. The program lists this folder next to the factory banks.

Five arrangement templates ship with the program:

| Template | Layout |
|---|---|
| Mono | One part, one voice. Lead lines. |
| Poly | One part, many voices. Classic polysynth. |
| Unison | One part, stacked voices. Fat leads. |
| Multitimbral | Six parts on six channels. Split setups. |
| Drum Kit (GM) | Six percussive parts on one channel. GM note maps. |

### File formats

| Format | Content |
|---|---|
| .yml | A full multi. Holds all six parts, effects and options. |
| .PRO | One Ambika program. The original hardware format. |
| .MUL | An Ambika multi. The original hardware multi format. |
| Host state | Your host saves the plugin state inside the project. |

Load any file by drag and drop onto the window. The File menu also loads and saves.
The program writes .PRO and .MUL files that the hardware reads back.

### What each format keeps

- .yml keeps everything. This includes filter card, drive and all effects.
- .PRO keeps one patch. It drops the card options. No Ambika byte exists for them.
- .MUL keeps the six patches and routing. It also drops card options.

<!--pagebreak-->

## 9. MIDI

### Channels and zones

Each part listens on its own MIDI channel. Set the channel on the Patch page.
A part can listen on all channels. Key zones split the keyboard between parts.

### Controllers

| Input | Effect |
|---|---|
| Note on / off | Triggers and releases voices. |
| Velocity | Scales the amplifier. Routs as a matrix source. |
| Channel pressure | Routs as a matrix source. |
| Pitch bend | Bends the pitch. Routs as a matrix source. |
| Mod wheel | Scales the last mod matrix row. |
| Expression | Routs as a matrix source. |

Your host automates every parameter. The parameters sort into labeled groups.

### MPE

Hellcat answers the MIDI Polyphonic Expression standard:

- Per-note pitch bend. Each note bends on its own.
- Per-note pressure. It routes as aftertouch per voice.
- Per-note slide. It routes as expression per voice.

Enable MPE on your MPE controller. Set its pitch range to match the controller setting.

### Clock sync

The program follows the host transport clock:

- Arp resolutions lock to the tempo.
- Step sequencers lock to the tempo.
- LFO rates under value 15 lock to the tempo.

The Standalone app follows an incoming MIDI clock on its MIDI input.

<!--pagebreak-->

## 10. Tips and Troubleshooting

### Tips

- Start from a template. It sets parts, zones and voice counts in one step.
- Keep the filter resonance under full on SMR4 and Polivoks. Both cards self-oscillate.
- IR3109 never self-oscillates. Use it for polite, chorused pads.
- Route Envelope 2 to "Filter Cutoff" for classic pluck sounds.
- Add slight "Spread" on the Patch page. It widens stacked voices.
- Two detuned saws plus "Sub Level" give a solid bass.
- Set "Crush" low for a lo-fi edge. Combine it with "Fuzz" for aggression.
- Save often as .parvati. Only this format keeps every setting.

### Troubleshooting

| Symptom | Check |
|---|---|
| No sound | Track monitor, MIDI routing, part voice count above zero. |
| Notes hang | Set "Arp Mode" to Off when no clock arrives. |
| Filter rings on its own | Lower resonance on SMR4, Ladder or Polivoks. |
| Tuning sounds wrong | Check the "Scale" setting on the Patch page. Set it to Off. |
| Effects absent | Check "FX1 Enable" and the "FX Mix" level. |
| Strong level drop | Check master EQ values. Set "FX EQ Mid" and "High" to 64. |
| Host shows old names | Rescan plugins. The host caches parameter names. |
| Crackles at high voice counts | Lower the voice slot counts. Close other audio programs. |

### Where to read more

- The tooltips cover every control. Rest the pointer on any knob.
- The parameter appendix lists every parameter with its range.
- The repository README covers building from source.
