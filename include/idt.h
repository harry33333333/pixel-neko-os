#ifndef IDT_H
#define IDT_H

struct trap_frame {
    unsigned int edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax; // Pushed by pushad
    unsigned int gs, fs, es, ds;                                // Pushed segment registers
    unsigned int int_no, err_code;                              // Interrupt vector and error code
    unsigned int eip, cs, eflags, user_esp, user_ss;            // Pushed by CPU on interrupt
};

void idt_init(void);
void idt_reload(void);
void pic_remap(void);
void isr_handler(struct trap_frame* frame);
void irq_handler(struct trap_frame* frame);

#endif
