#include "ffb.h"
#include <avr/io.h>
#include <avr/wdt.h>
#include <util/delay.h>
#include <avr/interrupt.h>

#define wdt_set_interrupt_only(value)   \
    __asm__ __volatile__ (  \
        "in __tmp_reg__,__SREG__" "\n\t"    \
        "cli" "\n\t"    \
        "wdr" "\n\t"    \
        "sts %0,%1" "\n\t"  \
        "out __SREG__,__tmp_reg__" "\n\t"   \
        "sts %0,%2" \
        : /* no outputs */  \
        : "M" (_SFR_MEM_ADDR(_WD_CONTROL_REG)), \
        "r" (_BV(_WD_CHANGE_BIT) | _BV(WDE)), \
			   "r" ((uint8_t) ((value & 0x08 ? _WD_PS3_MASK : 0x00) | \
					 _BV(WDIE) | (value & 0x07)) ) \
        : "r0"  \
    )

#define min(A, B) ((A < B) ? A : B)
#define max(A, B) ((A > B) ? A : B)
#define clamp(val, lower, upper) min(upper, max(lower, val))

static uint8_t PWM_on = 0;

static int16_t centerP = 0;
static int16_t centerI = 0;
static int16_t centerD = 0;
static uint16_t centerLookahead = 0;

static int8_t xCenter = 0;
static int8_t yCenter = 0;


static int16_t xIntegral = 0;
static int16_t yIntegral = 0;

void FFB_SetForceX(int8_t signedSpeed);
void FFB_SetForceY(int8_t signedSpeed);

void FFB_Init(void)
{
    DDRB |= (1 << PB5) | (1 << PB6) | (1 << PB4); // ADC X and Y, one direction enable for X
    DDRE |= (1 << PE6); // Other direction enable for X

    DDRD |= (1 << PD7); // One direction enable for Y
    DDRC |= (1 << PC6); // Other direction enable for Y

    PORTB &= ~(1 << PB4);
    PORTE &= ~(1 << PE6);

    PORTD &= ~(1 << PD7);
    PORTC &= ~(1 << PC6);

    wdt_set_interrupt_only(WDTO_30MS);
    FFB_Enable();
}

void FFB_Disable(void)
{
    FFB_SetForceX(0);
    FFB_SetForceY(0);

    TCCR1A = 0;
    TCCR1B = 0;

    PWM_on = 0;
}

void FFB_Enable(void)
{
    wdt_reset();

    TCCR1A = _BV(COM1A0) | _BV(COM1A1) | _BV(COM1B0) | _BV(COM1B1) | _BV(WGM10);
    TCCR1B = _BV(CS00);

    FFB_SetForceX(0);
    FFB_SetForceY(0);

    PWM_on = 1;
}

void FFB_SetForceY(int8_t signedSpeed)
{
    uint8_t direction = (signedSpeed >= 0) ? 1 : 0; //TODO: Does C99 guarantee the true condition here will result in 1?
    uint8_t notDirection = (signedSpeed >= 0) ? 0 : 1; //TODO: Does C99 guarantee !0 == 1?
    uint8_t pwmState;

    pwmState = (uint8_t)abs(signedSpeed);
    pwmState = 255 - (pwmState << 1);

    uint8_t portBState = PORTB & ~(1 << PB4);
    PORTB = portBState | (direction << PB4);

    uint8_t portEState = PORTE & ~(1 << PE6);
    PORTE = portEState | (notDirection << PE6);

    OCR1A = pwmState;
}

void FFB_SetForceX(int8_t signedSpeed)
{
    uint8_t direction = (signedSpeed >= 0) ? 1 : 0; //TODO: Does C99 guarantee the true condition here will result in 1?
    uint8_t notDirection = (signedSpeed >= 0) ? 0 : 1; //TODO: Does C99 guarantee !0 == 1?
    uint8_t pwmState;

    pwmState = (uint8_t)abs(signedSpeed);
    pwmState = 255 - (pwmState << 1);

    uint8_t portCState = PORTC & ~(1 << PC6);
    PORTC = portCState | (direction << PC6);

    uint8_t portDState = PORTD & ~(1 << PD7);
    PORTD = portDState | (notDirection << PD7);

    OCR1B = pwmState;
}

void FFB_SetPID(int16_t p, int16_t i, int16_t d, uint16_t lookahead)
{
    if (centerI != i)
    {
        xIntegral = 0;
        yIntegral = 0;
    }
    centerP = p;
    centerI = i;
    centerD = d;
    centerLookahead = lookahead;
}

void FFB_SetCenter(int8_t xCenterIn, int8_t yCenterIn)
{
    xCenter = xCenterIn;
    yCenter = yCenterIn;
}

void FFB_Update(int8_t xAxis, int8_t yAxis)
{
    const int16_t deadzone = 10;
    const int16_t stiction = 60;

    static int8_t lastX = 0;
    static int8_t lastY = 0;
    static int8_t firstRun = 1;

    static int16_t xLastVel = 0;
    static int16_t yLastVel = 0;

    if (firstRun)
    {
        lastX = xAxis;
        lastY = yAxis;
        firstRun = 0;
    }

    wdt_reset();
    if (PWM_on)
    {
        int16_t xVel = (xAxis - lastX) / 2 + xLastVel / 2;
        int16_t yVel = (yAxis - lastY) / 2 + yLastVel / 2;

        int16_t xError = xCenter - xAxis;
        int16_t yError = yCenter - yAxis;

        int16_t xProjectedError = xCenter - xAxis - xVel * centerLookahead;
        int16_t yProjectedError = yCenter - yAxis - yVel * centerLookahead;

        xIntegral += xError;
        yIntegral += yError;

        int16_t xForceP = clamp(((xError + xProjectedError) * centerP) / 32, -127, 127);
        int16_t yForceP = clamp(((yError + yProjectedError) * centerP) / 32, -127, 127);

        int16_t xForceI = clamp((xIntegral * centerI) / 256, -127, 127);
        int16_t yForceI = clamp((yIntegral * centerI) / 256, -127, 127);

        int16_t xForceD = clamp((xVel * centerD) / 8, -127, 127);
        int16_t yForceD = clamp((yVel * centerD) / 8, -127, 127);

        int16_t xTotalForce = xForceP + xForceI - xForceD;
        int16_t yTotalForce = yForceP + yForceI - yForceD;

        if (xTotalForce > deadzone)
        {
            xTotalForce += stiction;
        }
        if (xTotalForce < -deadzone)
        {
            xTotalForce -= stiction;
        }

        if (yTotalForce > deadzone)
        {
            yTotalForce += stiction;
        }
        if (yTotalForce < -deadzone)
        {
            yTotalForce -= stiction;
        }

        FFB_SetForceX((int8_t)clamp(xTotalForce, -127, 127));
        FFB_SetForceY((int8_t)clamp(yTotalForce, -127, 127));

        xLastVel = xVel;
        yLastVel = yVel;
    }

    lastX = xAxis;
    lastY = yAxis;
}

ISR(WDT_vect)
{
    FFB_SetForceX(0);
    FFB_SetForceY(0);
}
