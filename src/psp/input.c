#include <string.h>
#include "input.h"

void input_init(void)
{
    sceCtrlSetSamplingCycle(0);   /* 0 = amostragem sincronizada com o vblank */
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
}

void input_update(Input *in, float dt)
{
    SceCtrlData pad;
    sceCtrlReadBufferPositive(&pad, 1);

    unsigned int prev = in->held;
    in->held     = pad.Buttons;
    in->pressed  = pad.Buttons & ~prev;
    in->released = ~pad.Buttons & prev;

    /* O SDK entrega 0..255; convertemos pra -128..127 e zeramos a deadzone. */
    int x = (int)pad.Lx - 128;
    int y = (int)pad.Ly - 128;
    in->lx = (x > INPUT_DEADZONE || x < -INPUT_DEADZONE) ? x : 0;
    in->ly = (y > INPUT_DEADZONE || y < -INPUT_DEADZONE) ? y : 0;

    /*
     * Auto-repeat.
     *
     * O timer acompanha UM conjunto de botoes (rep_mask). Trocar de botao
     * reinicia o ciclo: sem isso, soltar CIMA no meio da fase rapida e apertar
     * BAIXO herdaria o ritmo acelerado e daria um salto imediato de varias
     * paginas na direcao oposta.
     */
    unsigned int rep = in->held & INPUT_REPEATABLE;
    in->repeated = in->pressed & INPUT_REPEATABLE;

    if (rep != in->rep_mask) {
        in->rep_mask    = rep;
        in->rep_timer   = 0.0f;
        in->rep_started = 0;
    } else if (rep) {
        in->rep_timer += dt;
        float threshold = in->rep_started ? INPUT_REPEAT_RATE : INPUT_REPEAT_DELAY;
        if (in->rep_timer >= threshold) {
            in->rep_timer  -= threshold;
            in->rep_started = 1;
            in->repeated   |= rep;
        }
    }

    /* Botoes fora do INPUT_REPEATABLE ainda entram em `repeated` na borda, para
     * o chamador poder usar so `repeated` e nunca precisar dos dois campos. */
    in->repeated |= in->pressed & ~INPUT_REPEATABLE;
}
