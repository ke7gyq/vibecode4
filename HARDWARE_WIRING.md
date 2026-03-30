# Hardware Connection Guide - ATSAMD21 PDM Microphone to RP2350

## Pinout Diagram

```
ATSAMD21 MEMS Microphone Module
╔══════════════════════════════════════╗
║ [1] CLK  ──────┐                    ║  Typical pinout
║ [2] DATA ──────┤                    ║  (verify your module)
║ [3] GND  ──────┤                    ║
║ [4] 3V3  ──────┤                    ║
╚══════════════════════════════════════╝
                 │
    ┌────────────┼────────────┐
    │            │            │
    ▼            ▼            ▼
   
   GPIO 6      GPIO 7        GND    3V3
   (Clock)     (Data)     (to Pico2)
```

## RP2350 Pico2 Connections

```
           ┌─────────────────────╖
           │ Raspberry Pi Pico 2 ║
           │                     ║
      ┌────┤ GPIO 6 (SPI CLK)   ║◄─── PDM Clock from microphone
      │    │                     ║
      │    │ GPIO 7 (SPI MOSI)  ║◄─── PDM Data from microphone
      │    │                     ║
      │    │ GND                ║◄─── Ground
      │    │                     ║
      │    │ 3V3                ║──► Power to microphone
      │    │                     ║
      │    └─[USB/Debug UART]   │
      │                         ║
      └─────────────────────────┘
```

## Complete Wiring

```
Component           Signal              RP2350 Pin      GPIO
───────────────────────────────────────────────────────────
ATSAMD21 Microphone CLK (output)  ───→  GPIO 6         GP6
ATSAMD21 Microphone DATA (output) ───→  GPIO 7         GP7
ATSAMD21 Microphone GND           ───→  GND            GND
ATSAMD21 Microphone 3V3           ←───  3V3            3V3
```

## Detailed Connection Steps

### 1. Prepare Wires
```
You need 4 connecting wires:
┌─────────────────────────────────────┐
│ • Red:    3V3 power (5-10cm)        │
│ • Black:  Ground (5-10cm)           │
│ • Green:  CLK/GPIO 6 (10-15cm)      │
│ • Yellow: DATA/GPIO 7 (10-15cm)     │
└─────────────────────────────────────┘

Tip: Use solid-core hookup wire or ribbon cable
```

### 2. Connect to ATSAMD21 Microphone
```
┌─ Microphone Module Pin 1 (CLK)  ← Green wire (GPIO 6)
├─ Microphone Module Pin 2 (DATA) ← Yellow wire (GPIO 7)
├─ Microphone Module Pin 3 (GND)  ← Black wire (Ground)
└─ Microphone Module Pin 4 (3V3)  ← Red wire (3V3 Power)

Make sure connections are secure (solder or pin headers)
```

### 3. Connect to RP2350 Pico2
```
RP2350 Pico2 GPIO Pinout (top view):
┌─────────────────────────────────────────────┐
│  1   2   3   4   5   6   7   8   9  10      │ ← Odd pins (left)
│ 40  39  38  37  36  35  34  33  32  31      │ ← Even pins (right)

GPIO mapping:
GPIO 6:  Pin 9  (3rd from top-left)   ← Green (CLK from microphone)
GPIO 7:  Pin 10 (4th from top-left)   ← Yellow (DATA from microphone)
GND:     Pin 3, 8, 13, 18, 23, 28, 33, 38 ← Black (Ground)
3V3:     Pin 36 (2nd from top-right) ← Red (3V3 Power out)

Verify pinout against your Pico2 silk-screen!
```

## Physical Layout Recommendations

```
┌────────────────────────────────────────┐
│  ATSAMD21 Microphone Module            │
│  ┌──────────────────────────────────┐  │
│  │ [1-CLK] [2-DATA] [3-GND] [4-3V3] │  │
│  └──────────────────────────────────┘  │
│      │      │       │      │           │
│      │      │       │      └──→ RP2350 3V3  (Pin 36)
│      │      │       └─────→ RP2350 GND     (Any GND)
│      │      └──────→ RP2350 GPIO7/[Pin 10]
│      └────────────→ RP2350 GPIO6/[Pin 9]
│                                         │
│  Spacing: Keep wires ≤ 15cm, avoid     │
│           crossing power/clock wires    │
└────────────────────────────────────────┘
```

## Electrical Considerations

### Power
- **3V3 Supply**: RP2350 can source ~100mA total
- **ATSAMD21 Current**: Typically 5-20mA
- **Decoupling**: Add 100nF capacitor near microphone 3V3 pin

### Signal Integrity
- **Clock Line (GPIO 6)**
  - Impedance: ~50Ω (coax ideal, unshielded OK for <15cm)
  - Voltage: 3.3V CMOS logic
  - Frequency: ~4 MHz (harmonics to ~20 MHz)

- **Data Line (GPIO 7)**
  - Voltage: 3.3V CMOS logic at ~4 MHz clock rate
  - Capacitive load: Keep wires short to minimize

### Noise Reduction
1. ✓ Keep wires short (<15cm)
2. ✓ Group GND wires together
3. ✓ Avoid routing near power switching circuits
4. ✓ Use shielded cable if possible (shield to GND)
5. ✓ Add 100nF decoupling cap to microphone 3V3

## Connector Options

### Option 1: Direct Soldering (Most Reliable)
```
Solder 4 wires directly to:
- Microphone module pads
- RP2350 GPIO headers
+ Most reliable
- Permanent connection
```

### Option 2: Female Headers (Flexible)
```
├─ Solder male header pins to Pico2
├─ Solder female header to microphone
├─ Use female-to-female jumper wires
+ Easy to reconfigure
- More connections = more noise potential
```

### Option 3: Breadboard (Prototyping)
```
├─ Insert Pico2 into breadboard
├─ Use hookup wire or jumper wires
├─ Connect microphone via breadboard
+ Quick testing
- Poor signal integrity at high frequencies
```

## Testing the Connection

### LED Blink Test (Verify power)
Power LED on microphone should be on/steady

### Continuity Test (Multimeter)
```
Multimeter Settings: Continuity (Ω or beep mode)

Test:  Probe CLK wire at both ends
       Should beep/show ~0Ω (connected)
       
       Probe Data wire at both ends
       Should beep/show ~0Ω
       
       Probe GND at both ends
       Should beep/show ~0Ω if same GND plane
```

### Voltage Test
```
Multimeter Settings: DC Voltage (3V to 20V range)

With Pico2 powered:
  GPIO 6 (CLK):  Should show ~1.65V average
                 (oscillating between 0V and 3.3V)
  
  GPIO 7 (Data): Should show ~1.65V average
                 (varies with audio - 0-3.3V)
  
  GND:           Should show 0V reference
  
  3V3:           Should show 3.3V steady
```

### Oscilloscope View (Optional)
```
If you have an oscilloscope:

Channel 1 (GPIO 6 - CLK):
  ┌─ Rectangular wave
  ├─ Frequency: ~4 MHz
  ├─ Period: ~250 ns
  ├─ Amplitude: 0-3.3V
  └─ Duty cycle: ~50%
     
  Expected pattern:
  3.3V ┌─┐   ┌─┐   ┌─┐
       │ └─┬─┘ └─┬─┘ └─
   0V  └───┴─────┴─────
       └─────────────→ time (250ns per division)

Channel 2 (GPIO 7 - Data):
  ┌─ Random bit pattern at CLK frequency
  ├─ Sampled at each CLK rising edge
  ├─ Amplitude: 0-3.3V
  └─ Changes every ~250 ns
  
  Expected pattern:
  3.3V ┌─┐   ┐   ┌───┐
       │ └─┬─┘ └─┬┘   └
   0V  └───┴─────┴─────
       └─────────────→ time
       (Random 1-bit stream)
```

## Troubleshooting Connections

| Symptom | Likely Cause | Solution |
|---------|------|----------|
| No power to microphone | Reversed 3V3/GND | Verify red=3V3, black=GND |
| No clock signal on GPIO 6 | Wire disconnected | Check continuity; resolder |
| No data signal on GPIO 7 | Wire disconnected or microphone stuck | Check continuity; try CLK 10kΩ pull-up |
| Very noisy data | Long wires; routing near interference | Use shielded cable; reduce length |
| Intermittent operation | Loose connection | Resolder all connections; use strain relief |

## Safety Considerations

```
DO:
✓ Use 3.3V only (Pico2 GPIO not 5V tolerant)
✓ Connect GND first/last
✓ Use appropriate wire gauge (22-26 AWG)
✓ Keep connections away from moving parts

DON'T:
✗ Use 5V power (will damage Pico2)
✗ Connect backwards (GND to 3V3)
✗ Use extremely thin wire (<28 AWG)
✗ Leave exposed solder joints touching
```

## Verification Checklist

Before applying power:
- [ ] All 4 wires connected correctly
- [ ] No exposed wires touching each other
- [ ] 3.3V connected to 3V3 (not GND)
- [ ] GND connected to GND
- [ ] CLK wire to GPIO 6
- [ ] DATA wire to GPIO 7
- [ ] Microphone header pins firmly seated

After applying power:
- [ ] Microphone power LED on (if equipped)
- [ ] No burning smell
- [ ] Pico2 still responsive

---

**Connection complete!** Your ATSAMD21 microphone is now wired to the RP2350 Pico2.  
The LED should blink, and you should see audio messages on the UART console.

