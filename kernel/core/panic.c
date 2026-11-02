#include "panic.h"
#include "idt.h"
#include "boot_framebuffer.h"
#include "log.h"
#include "port.h"
#include <stdint.h>
#include <stdbool.h>

// 24x24 Classic Mac OS Burning Bomb Pixel Art
static const char* bomb_art[24] = {
    "            * *         ",
    "           *   *        ",
    "            ***         ",
    "             |          ",
    "           --+--        ",
    "          |     |       ",
    "        XX.......XX     ",
    "       X...........X    ",
    "      X..XX.........X   ",
    "     X..X  X.........X  ",
    "    X...X  X..........X ",
    "    X....XX...........X ",
    "    X.................X ",
    "    X.................X ",
    "    X.................X ",
    "     X...............X  ",
    "      X.............X   ",
    "       X...........X    ",
    "        XX.......XX     ",
    "          XXXXXXX       ",
    "                        ",
    "                        ",
    "                        ",
    "                        "
};

static void hex64_to_str(uint64_t val, char* out) {
    const char hex[] = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 16; i++) {
        out[2 + i] = hex[(val >> ((15 - i) * 4)) & 0xF];
    }
    out[18] = '\0';
}

static void sys_reboot_8042(void) {
    log_printf("[PANIC] User triggered reboot from bomb window. Resetting CPU...\n");
    // Pulse reset line via keyboard controller
    uint8_t temp;
    do {
        temp = inb(0x64);
        if (temp & 1) (void)inb(0x60);
    } while (temp & 2);
    outb(0x64, 0xFE);
    for (;;) { __asm__ volatile ("hlt"); }
}

void panic_show_exception(struct trap_frame* frame, const char* name) {
    // Disable interrupts unconditionally
    __asm__ volatile ("cli");

    uint64_t cr2 = 0;
    if (frame && frame->int_no == 14) {
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    }

    log_printf("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    log_printf("  *** KERNEL PANIC: %s ***\n", name ? name : "FATAL EXCEPTION");
    if (frame) {
        log_printf("  Vector: %llu, ErrorCode: 0x%llx\n", frame->int_no, frame->err_code);
        log_printf("  RIP: 0x%016llx, CS: 0x%04llx, RFLAGS: 0x%016llx\n", frame->rip, frame->cs, frame->rflags);
        log_printf("  RSP: 0x%016llx, SS: 0x%04llx, CR2: 0x%016llx\n", frame->rsp, frame->ss, cr2);
        log_printf("  RAX: 0x%016llx, RBX: 0x%016llx, RCX: 0x%016llx\n", frame->rax, frame->rbx, frame->rcx);
        log_printf("  RDX: 0x%016llx, RSI: 0x%016llx, RDI: 0x%016llx\n", frame->rdx, frame->rsi, frame->rdi);
        log_printf("  RBP: 0x%016llx, R8:  0x%016llx, R9:  0x%016llx\n", frame->rbp, frame->r8, frame->r9);
    }
    log_printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");

    if (!g_main_surface.framebuffer_virt) {
        for (;;) { __asm__ volatile ("pause"); }
    }

    uint32_t c_black  = boot_framebuffer_make_color(0, 0, 0);
    uint32_t c_white  = boot_framebuffer_make_color(255, 255, 255);
    uint32_t c_gray   = boot_framebuffer_make_color(200, 200, 200);
    uint32_t c_dgray  = boot_framebuffer_make_color(100, 100, 100);
    uint32_t c_orange = boot_framebuffer_make_color(255, 128, 0);
    uint32_t c_red    = boot_framebuffer_make_color(180, 0, 0);

    int win_w = 560;
    int win_h = 240;
    int bx = ((int)g_main_surface.width - win_w) / 2;
    int by = ((int)g_main_surface.height - win_h) / 2;

    // Drop Shadow
    boot_framebuffer_fill_rect(bx + 8, by + 8, win_w, win_h, c_dgray);

    // Box body (Classic Mac Platinum Light Gray)
    boot_framebuffer_fill_rect(bx, by, win_w, win_h, c_gray);

    // Classic Mac Double Outline (Black)
    boot_framebuffer_fill_rect(bx, by, win_w, 2, c_black);
    boot_framebuffer_fill_rect(bx, by + win_h - 2, win_w, 2, c_black);
    boot_framebuffer_fill_rect(bx, by, 2, win_h, c_black);
    boot_framebuffer_fill_rect(bx + win_w - 2, by, 2, win_h, c_black);

    boot_framebuffer_fill_rect(bx + 4, by + 4, win_w - 8, 1, c_black);
    boot_framebuffer_fill_rect(bx + 4, by + win_h - 5, win_w - 8, 1, c_black);
    boot_framebuffer_fill_rect(bx + 4, by + 4, 1, win_h - 8, c_black);
    boot_framebuffer_fill_rect(bx + win_w - 5, by + 4, 1, win_h - 8, c_black);

    // Draw 24x24 Burning Bomb Icon
    int ix = bx + 20, iy = by + 20;
    for (int r = 0; r < 20; r++) {
        for (int c = 0; c < 24; c++) {
            char ch = bomb_art[r][c];
            if (ch == '*') boot_framebuffer_putpixel(ix + c, iy + r, c_orange);
            else if (ch == '|') boot_framebuffer_putpixel(ix + c, iy + r, c_dgray);
            else if (ch == '+') boot_framebuffer_putpixel(ix + c, iy + r, c_black);
            else if (ch == 'X' || ch == '.') boot_framebuffer_putpixel(ix + c, iy + r, c_black);
        }
    }
    // Bomb shine reflection highlight
    boot_framebuffer_putpixel(ix + 8, iy + 9, c_white);
    boot_framebuffer_putpixel(ix + 9, iy + 9, c_white);
    boot_framebuffer_putpixel(ix + 8, iy + 10, c_white);

    // Error text lines
    int tx = bx + 64;
    int ty = by + 20;
    boot_framebuffer_draw_string(tx, ty, "Sorry, a system error occurred.", c_black, c_gray);
    ty += 20;

    boot_framebuffer_draw_string(tx, ty, name ? name : "Fatal Exception Encountered", c_red, c_gray);
    ty += 24;

    char hbuf[32];
    char line[96];

    if (frame) {
        hex64_to_str(frame->rip, hbuf);
        int p = 0;
        const char* l1 = "RIP: ";
        while (*l1) line[p++] = *l1++;
        for (int i = 0; hbuf[i]; i++) line[p++] = hbuf[i];
        line[p++] = ' '; line[p++] = 'C'; line[p++] = 'S'; line[p++] = ':'; line[p++] = ' ';
        hex64_to_str(frame->cs, hbuf);
        for (int i = 0; i < 6 && hbuf[i]; i++) line[p++] = hbuf[i];
        line[p] = '\0';
        boot_framebuffer_draw_string(tx, ty, line, c_black, c_gray);
        ty += 18;

        hex64_to_str(frame->rsp, hbuf);
        p = 0;
        const char* l2 = "RSP: ";
        while (*l2) line[p++] = *l2++;
        for (int i = 0; hbuf[i]; i++) line[p++] = hbuf[i];
        line[p++] = ' '; line[p++] = 'E'; line[p++] = 'r'; line[p++] = 'r'; line[p++] = ':'; line[p++] = ' ';
        hex64_to_str(frame->err_code, hbuf);
        for (int i = 0; i < 8 && hbuf[i]; i++) line[p++] = hbuf[i];
        line[p] = '\0';
        boot_framebuffer_draw_string(tx, ty, line, c_black, c_gray);
        ty += 18;

        if (frame->int_no == 14) {
            hex64_to_str(cr2, hbuf);
            p = 0;
            const char* l3 = "CR2 (Page Fault Linear Target): ";
            while (*l3) line[p++] = *l3++;
            for (int i = 0; hbuf[i]; i++) line[p++] = hbuf[i];
            line[p] = '\0';
            boot_framebuffer_draw_string(tx, ty, line, c_red, c_gray);
            ty += 18;
        }
    }

    // [ Restart Computer ] 3D Button
    int btn_w = 170;
    int btn_h = 26;
    int btn_x = bx + (win_w - btn_w) / 2;
    int btn_y = by + win_h - 40;

    boot_framebuffer_fill_rect(btn_x, btn_y, btn_w, btn_h, c_gray);
    boot_framebuffer_fill_rect(btn_x, btn_y, btn_w, 2, c_white);
    boot_framebuffer_fill_rect(btn_x, btn_y, 2, btn_h, c_white);
    boot_framebuffer_fill_rect(btn_x + btn_w - 2, btn_y, 2, btn_h, c_black);
    boot_framebuffer_fill_rect(btn_x, btn_y + btn_h - 2, btn_w, 2, c_black);
    boot_framebuffer_draw_string(btn_x + 14, btn_y + 5, "Restart Computer", c_black, c_gray);

    // Halt spin loop and listen for any keyboard press to trigger safe reboot
    for (;;) {
        if (inb(0x64) & 1) {
            (void)inb(0x60);
            sys_reboot_8042();
        }
        __asm__ volatile ("pause");
    }
}

void panic_halt(const char* message) {
    panic_show_exception(NULL, message);
}
