#ifndef EREADER_INPUT_H
#define EREADER_INPUT_H

#include <pspctrl.h>

/*
 * Wrapper sobre sceCtrl com deteccao de borda e auto-repeat.
 *
 * O SDK so entrega o estado atual dos botoes; "acabou de apertar" (edge) e o
 * que a UI realmente precisa, senao o cursor anda 60 casas por segundo.
 *
 * O auto-repeat esta AQUI, e nao no menu como no midnight, porque num leitor
 * ele e necessario tambem na virada de pagina: segurar R1 para avancar varias
 * paginas e o gesto natural, e sem repeat cada pagina exige um toque.
 */
typedef struct {
    unsigned int held;      /* segurando agora             */
    unsigned int pressed;   /* borda de subida neste frame */
    unsigned int released;  /* borda de descida            */
    unsigned int repeated;  /* borda de subida OU repeticao por segurar */
    int lx, ly;             /* analogico centrado: -128..127 */

    /* estado interno do auto-repeat */
    unsigned int rep_mask;
    float rep_timer;
    int   rep_started;
} Input;

void input_init(void);

/* dt em segundos, usado pelo auto-repeat. */
void input_update(Input *in, float dt);

/* Deadzone aplicada: o analogico do PSP raramente descansa exatamente no zero. */
#define INPUT_DEADZONE 32

/* Espera antes de comecar a repetir, e intervalo entre repeticoes. Calibrado
 * para virada de pagina: rapido o suficiente para folhear, lento o suficiente
 * para nao passar da pagina que se queria. */
#define INPUT_REPEAT_DELAY 0.35f
#define INPUT_REPEAT_RATE  0.10f

/* Botoes que participam do auto-repeat. Os de confirmacao (X, O, triangulo,
 * quadrado, start, select) ficam de fora de proposito: repetir "abrir livro"
 * ou "voltar" e sempre um bug, nunca a intencao do usuario. */
#define INPUT_REPEATABLE (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | \
                          PSP_CTRL_RIGHT | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)

#endif
