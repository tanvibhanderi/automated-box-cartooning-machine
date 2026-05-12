/*
 * Automated Box Cartooning Machine — Firmware
 * Platform : TM4C123GH6PM (Tiva C LaunchPad)
 * Author   : Tanvi Bhanderi
 * Course   : MENG-2500 Mechatronics Project, Humber Polytechnic — Winter 2025
 *
 * Description:
 *   Controls a 4-phase automated box folding machine using:
 *   - NEMA 17 stepper motor (revolving disc transport)
 *   - MG996R servo motors x2 (flap folding)
 *   - 3x pneumatic DCV solenoid valves (pickup & ejection)
 *   - Venturi vacuum generator (suction cup)
 *
 * Pin Mapping:
 *   Port B: Stepper (PB6=STEP, PB7=DIR), DCVs (PB0, PB1, PB5)
 *   Port A: Encoder inputs (PA3, PA4, PA5)
 *   Port D: Pneumatic valve outputs (PD0, PD1, PD2)
 *   Port F: Debug LEDs (PF1, PF2)
 *
 * Operation Cycle:
 *   Phase 1 → Box pickup (piston + vacuum)
 *   Phase 2 → Flap pre-fold (servo arms flatten top flaps)
 *   Phase 3 → Lid close (servo L-rod presses lid down)
 *   Phase 4 → Box ejection (cylinder + suction cup removes box)
 *   → Disc resets → Repeat
 *
 * Build: Keil uVision / TI Code Composer Studio
 */

#include "TM4C123GH6PM.h"
#include <stdint.h>

// ─────────────────────────────────────────────
// Pin Definitions
// ─────────────────────────────────────────────

// DCV solenoid valves — Port B (active-low)
#define DCV1  0x01   // PB0 — vacuum/pickup cylinder
#define DCV2  0x02   // PB1 — ejection cylinder
#define DCV3  0x20   // PB5 — flap fold cylinder

// Stepper motor — Port B
#define STEP_PIN     0x40   // PB6
#define DIR_PIN      0x80   // PB7
#define DEG_PER_STEP 0.45   // For 400 steps/rev (half-step mode)

// Timing
#define DEFAULT_DELAY 32

// ─────────────────────────────────────────────
// Global Variables
// ─────────────────────────────────────────────
int delay_time = DEFAULT_DELAY;

// ─────────────────────────────────────────────
// Port B Init — Stepper motor + DCV solenoids
// ─────────────────────────────────────────────
void PortB_Init(void) {
    SYSCTL->RCGCGPIO |= 0x02;
    while ((SYSCTL->PRGPIO & 0x02) == 0);

    GPIOB->LOCK  =  0x4C4F434B;
    GPIOB->CR   |=  0xFF;
    GPIOB->AMSEL &= ~0xFF;
    GPIOB->PCTL  &= ~0xFFFFFFFF;
    GPIOB->DIR  |=  0xFF;
    GPIOB->AFSEL &= ~0xFF;
    GPIOB->DEN  |=  0xFF;
}

// ─────────────────────────────────────────────
// Port D Init — Pneumatic valve outputs
// ─────────────────────────────────────────────
void PortD_Init(void) {
    SYSCTL->RCGCGPIO |= 0x08;
    GPIOD->LOCK  =  0x4C4F434B;
    GPIOD->CR   |=  0x07;
    while ((SYSCTL->RCGCGPIO & 0x08) == 0);

    GPIOD->DIR  |=  0x07;
    GPIOD->DEN  |=  0x07;
}

// ─────────────────────────────────────────────
// GPIO Init — Encoder inputs (Port A) + Debug LEDs (Port F)
// ─────────────────────────────────────────────
void GPIO_Init(void) {
    SYSCTL->RCGCGPIO |= 0x21;
    while ((SYSCTL->PRGPIO & 0x21) == 0);

    // Port A — encoder inputs (PA3, PA4, PA5)
    GPIOA->DIR  &= ~(0x38);
    GPIOA->DEN  |=  0x38;

    // Port F — debug LEDs (PF1=Red, PF2=Blue)
    GPIOF->DIR  |=  0x06;
    GPIOF->DEN  |=  0x06;
}

// ─────────────────────────────────────────────
// Microsecond Delay — Timer1A hardware timer
// ─────────────────────────────────────────────
void Delay_MicroSecond(int time) {
    int i;
    SYSCTL->RCGCTIMER |= 2;
    TIMER1->CTL   = 0;
    TIMER1->CFG   = 0x04;
    TIMER1->TAMR  = 0x02;
    TIMER1->TAILR = 16 - 1;
    TIMER1->ICR   = 0x1;
    TIMER1->CTL  |= 0x01;

    for (i = 0; i < time; i++) {
        while ((TIMER1->RIS & 0x1) == 0);
        TIMER1->ICR = 0x1;
    }
}

// ─────────────────────────────────────────────
// Millisecond Delay — Software loop
// ─────────────────────────────────────────────
void Delay_ms(int ms) {
    int i, j;
    for (i = 0; i < ms; i++)
        for (j = 0; j < 3180; j++);
}

// ─────────────────────────────────────────────
// Servo Control — PWM pulse via bit-banging
// pulseWidth: microseconds (1000=0°, 2000=180°)
// pin: GPIOB bit position (2=PB2, 3=PB3)
// ─────────────────────────────────────────────
void Servo_Set_Angle(int pulseWidth, uint8_t pin) {
    for (int i = 0; i < 50; i++) {
        GPIOB->DATA |=  (1 << pin);
        Delay_MicroSecond(pulseWidth);
        GPIOB->DATA &= ~(1 << pin);
        Delay_MicroSecond(20000 - pulseWidth);
    }
}

// ─────────────────────────────────────────────
// Angle to Pulse Width Conversion
// Maps 0–180° → 1000–2800 µs
// ─────────────────────────────────────────────
int AngleToPulse(int degree) {
    return 1000 + ((degree * 1800) / 180);
}

// ─────────────────────────────────────────────
// DCV Solenoid Valve Control (active-low)
// dcv_mask: bitmask of valves to activate
// ─────────────────────────────────────────────
void DCV_Set(uint8_t dcv_mask) {
    uint8_t mask = (1 << 4) | (1 << 1) | (1 << 5);   // PB4, PB1, PB5
    GPIOB->DATA = (GPIOB->DATA & ~mask) | (~dcv_mask & mask);
}

// ─────────────────────────────────────────────
// Stepper Motor Rotation
// angle: degrees to rotate
// speed_delay_ms: step delay (lower = faster)
// direction: 1=CW, 0=CCW
// ─────────────────────────────────────────────
void RotateMotor(float angle, uint32_t speed_delay_ms, int direction) {
    uint32_t steps = (uint32_t)(angle / DEG_PER_STEP);

    if (direction == 1)
        GPIOB->DATA |=  DIR_PIN;   // Clockwise
    else
        GPIOB->DATA &= ~DIR_PIN;   // Counter-clockwise

    for (uint32_t i = 0; i < steps; i++) {
        GPIOB->DATA |=  STEP_PIN;
        Delay_ms(speed_delay_ms);
        GPIOB->DATA &= ~STEP_PIN;
        Delay_ms(speed_delay_ms);
    }
}

// ─────────────────────────────────────────────
// Manual Fine-Adjust (keypad tuning)
// ─────────────────────────────────────────────
void Manual_Adjust_Steps(int steps) {
    for (int i = 0; i < steps; i++) {
        GPIOB->DATA |=  STEP_PIN;
        Delay_ms(delay_time);
        GPIOB->DATA &= ~STEP_PIN;
        Delay_ms(delay_time);
    }
}

// ─────────────────────────────────────────────
// Main — 4-Phase Cartooning Cycle
// ─────────────────────────────────────────────
int main(void) {

    // Initialize all peripherals
    PortB_Init();
    GPIO_Init();
    PortD_Init();

    // ── Startup: DCV3 ON (vacuum ready), servos to home position ──
    DCV_Set(0x20);                          // DCV3 ON — vacuum ON
    Delay_ms(500);
    Servo_Set_Angle(AngleToPulse(140), 2);  // Servo 1 → home (140°)
    Servo_Set_Angle(AngleToPulse(0),   3);  // Servo 2 → home (0°)

    // ── Main Operation Loop ──
    while (1) {

        // ── PHASE 1: Box Pickup ──────────────────────────────────
        // Servo 1 opens, Servo 2 pre-positions
        // DCV2 + DCV3 activate: vacuum + pickup cylinder extends
        Servo_Set_Angle(AngleToPulse(0),   2);   // Servo 1 open
        Delay_ms(500);
        Servo_Set_Angle(AngleToPulse(100), 3);   // Servo 2 pre-position
        DCV_Set(0x22);                            // DCV2 + DCV3 ON
        Delay_ms(500);

        // ── PHASE 2: Flap Pre-Fold ───────────────────────────────
        // DCV1 + DCV2 activated: cylinder pushes box onto disc
        // Flap arms flatten top flaps without sealing
        DCV_Set(0x12);                            // DCV1 + DCV2 ON
        Delay_ms(2000);

        // ── PHASE 3: Lid Close ───────────────────────────────────
        // Servo L-rod presses lid down, disc rotates 67.5°
        DCV_Set(0x10);                            // DCV1 ON only
        Servo_Set_Angle(AngleToPulse(0),   3);   // Servo 2 lid press
        RotateMotor(67.5, 16, 1);                 // Disc rotates CW 67.5°

        // ── PHASE 4: Box Ejection ────────────────────────────────
        // Servo 1 closes, DCV3 reactivates for ejection cylinder
        // Disc resets for next cycle
        Servo_Set_Angle(AngleToPulse(140), 2);   // Servo 1 close
        DCV_Set(0x20);                            // DCV3 ON — ejection

        Delay_ms(1000);
        RotateMotor(67.5, 16, 1);                 // Disc resets
        Delay_ms(500);

        // ── Cycle complete → restart from Phase 1 ──
    }
}
