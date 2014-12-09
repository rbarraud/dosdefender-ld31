#include "init.h"
#include "print.h"
#include "joystick.h"
#include "vga.h"
#include "rand.h"
#include "time.h"
#include "alloc.h"
#include "keyboard.h"
#include "speaker.h"

#define SCALE              1000
#define BACKGROUND         17
#define PLAYER             14
#define BULLET_SPEED       3
#define PARTICLE_MAX_AGE   50

typedef unsigned int tick_t;
typedef void (*ai_t)(int id);
typedef void (*power_t)(void);

static tick_t ticks;
static unsigned score;
static unsigned best_score;
static struct speaker speaker;

struct ship {
    int32_t x, y, dx, dy;
    tick_t last_fire;
    ai_t ai;
    struct sample *fx_fire;
    uint16_t score;
    uint16_t hp, hp_max;
    uint8_t radius;
    uint8_t fire_delay;
    uint8_t fire_damage;
    uint8_t drop_rate;
    uint8_t color_a, color_b;
};

struct bullet {
    int32_t x, y, dx, dy;
    tick_t birthtick;
    uint8_t color;
    uint8_t damage;
    bool alive;
};

struct particle {
    int32_t x, y;
    tick_t birthtick;
    bool alive;
};

struct powerup {
    int32_t x, y;
    tick_t birthtick;
    power_t power;
    bool alive;
    uint8_t color;
};

static struct bullet *bullets;
static size_t bullets_max = 32;

static struct particle *particles;
static size_t particles_max = 64;

static struct ship *ships;
static size_t ships_max = 12;

static struct powerup *powerups;
static size_t powerups_max = 8;

static bool joystick_detected()
{
    struct joystick joystick;
    joystick_read(&joystick);
    return joystick.axis[0] != 0 || joystick.axis[1] != 0;
}

static void burn(int32_t x, int32_t y);
static void ship_draw(int id, bool clear);
static void powerup_random(int id);

static void bullet_draw(int i, bool clear)
{
    struct point c = {bullets[i].x / SCALE, bullets[i].y / SCALE};
    vga_pixel(c, clear ? BACKGROUND : bullets[i].color);
}

static bool bullet_in_ship(int bi, int si)
{
    return bullets[bi].x >= ships[si].x - (SCALE * ships[si].radius) &&
           bullets[bi].y >= ships[si].y - (SCALE * ships[si].radius) &&
           bullets[bi].x <= ships[si].x + (SCALE * ships[si].radius) &&
           bullets[bi].y <= ships[si].y + (SCALE * ships[si].radius);
}

static void bullet_step(int i)
{
    bullets[i].x += bullets[i].dx;
    bullets[i].y += bullets[i].dy;
    if (bullets[i].x < 0 || bullets[i].x > VGA_PWIDTH * SCALE ||
        bullets[i].y < 0 || bullets[i].y > VGA_PHEIGHT * SCALE)
        bullets[i].alive = false;
    for (int id = 0; id < ships_max; id++) {
        if (ships[id].hp > 0 && ships[id].color_b != bullets[i].color) {
            if (bullet_in_ship(i, id)) {
                if (ships[id].hp >= bullets[i].damage)
                    ships[id].hp -= bullets[i].damage;
                else
                    ships[id].hp = 0;
                bullets[i].alive = false; // absorb
                if (ships[id].hp == 0) {
                    for (int j = 0; j < 10; j++)
                        burn(ships[id].x, ships[id].y);
                    ship_draw(id, true);
                    if (ships[0].hp > 0)
                        score += ships[id].score;
                    if (id != 0)
                        powerup_random(id);
                    speaker_play(&speaker, &fx_explode);
                } else if (id == 0) {
                    speaker_play(&speaker, &fx_hit);
                }
                break;
            }
        }
    }
}

static int ship_fire(int i)
{
    if (ships[i].last_fire + ships[i].fire_delay > ticks)
        return -1;
    ships[i].last_fire = ticks;
    int choice = 0;
    for (int i = 0; i < bullets_max; i++) {
        if (!bullets[i].alive) {
            choice = i;
            break;
        } else if (bullets[i].birthtick < bullets[choice].birthtick) {
            choice = i;
        }
    }
    if (bullets[choice].alive)
        bullet_draw(choice, true);
    bullets[choice].x = ships[i].x + ships[i].dx / 100;
    bullets[choice].y = ships[i].y + ships[i].dy / 100;
    bullets[choice].dx = ships[i].dx * BULLET_SPEED;
    bullets[choice].dy = ships[i].dy * BULLET_SPEED;
    bullets[choice].color = ships[i].color_b;
    bullets[choice].birthtick = ticks;
    bullets[choice].damage = ships[i].fire_damage;
    bullets[choice].alive = true;
    speaker_play(&speaker, ships[i].fx_fire);
    return choice;
}

static void particle_draw(int i, bool clear)
{
    struct point c = {particles[i].x / SCALE, particles[i].y / SCALE};
    if (clear) {
        vga_pixel(c, BACKGROUND);
    } else {
        int age = ticks - particles[i].birthtick;
        vga_pixel(c, age > PARTICLE_MAX_AGE * 3 / 4
                  ? (randn(5) + 24)    // smoke
                  : (randn(5) + 40));  // fire
    }
}

static void particle_step(int i)
{
    if (ticks - particles[i].birthtick > PARTICLE_MAX_AGE) {
        particles[i].alive = false;
        particle_draw(i, true);
    } else {
        int speed = 2;
        particles[i].x += randn(SCALE * speed) - (SCALE * speed / 2);
        particles[i].y += randn(SCALE * speed) - (SCALE * speed / 2);
    }
}

static void burn(int32_t x, int32_t y)
{
    int choice = 0;
    for (int i = 0; i < particles_max; i++) {
        if (!particles[i].alive) {
            choice = i;
            break;
        } else if (particles[i].birthtick < particles[choice].birthtick) {
            choice = i;
        }
    }
    if (particles[choice].alive)
        particle_draw(choice, true);
    particles[choice].alive = true;
    particles[choice].x = x;
    particles[choice].y = y;
    particles[choice].birthtick = ticks;
}

static void ship_draw(int id, bool clear)
{
    struct point c = {ships[id].x / SCALE, ships[id].y / SCALE};
    int r = ships[id].radius;
    for (int i = -r + 1; i < r; i++) {
        struct point ha = {ships[id].x / SCALE - i, ships[id].y / SCALE - r};
        struct point hb = {ships[id].x / SCALE - i, ships[id].y / SCALE + r};
        struct point va = {ships[id].x / SCALE - r, ships[id].y / SCALE - i};
        struct point vb = {ships[id].x / SCALE + r, ships[id].y / SCALE - i};
        vga_pixel(ha, clear ? BACKGROUND : ships[id].color_a);
        vga_pixel(va, clear ? BACKGROUND : ships[id].color_a);
        vga_pixel(hb, clear ? BACKGROUND : ships[id].color_a);
        vga_pixel(vb, clear ? BACKGROUND : ships[id].color_a);
    }
    struct point d = {c.x + ships[id].dx / 10, c.y + ships[id].dy / 10};
    if (id == 0)
        vga_pixel(d, clear ? BACKGROUND : WHITE);
}

static void ship_step(int i)
{
    ships[i].x += ships[i].dx;
    ships[i].y += ships[i].dy;
    ships[i].dx = (ships[i].dx * 99) / 100;
    ships[i].dy = (ships[i].dy * 99) / 100;
    if (ships[i].hp < ships[i].hp_max / 2) {
        if ((randn(ships[i].hp < 1 ? 1 : ships[i].hp) < 10))
            burn(ships[i].x, ships[i].y);
    }
}

static void powerup_draw(int i, bool clear)
{
    int x = powerups[i].x / SCALE;
    int y = powerups[i].y / SCALE;
    uint8_t color;
    tick_t tick = ticks;
    if (clear) {
        color = BACKGROUND;
        tick--;
    } else {
        color = powerups[i].color;
    }
    int size = 0 + (tick / 8) % 3;
    vga_line((struct point){x - size, y},
             (struct point){x + size, y}, color);
    vga_line((struct point){x, y - size},
             (struct point){x, y + size}, color);
}

static void powerup_step(int i)
{
    int px = powerups[i].x / SCALE;
    int py = powerups[i].y / SCALE;
    int sx = ships[0].x / SCALE;
    int sy = ships[0].y / SCALE;
    if (ships[0].hp > 0 &&
        px >= sx - 4 &&
        py >= sy - 4 &&
        px <= sx + 4 &&
        py <= sy + 4) {
        powerups[i].power();
        powerups[i].alive = false;
        speaker_play(&speaker, &fx_powerup);
    }
}

static int powerup_drop(int32_t x, int32_t y)
{
    int choice = -1;
    for (int i = 0; i < powerups_max; i++) {
        if (!powerups[i].alive) {
            choice = i;
            break;
        }
    }
    if (choice >= 0) {
        powerups[choice].x = x;
        powerups[choice].y = y;
        powerups[choice].birthtick = ticks;
        powerups[choice].alive = true;
    }
    return choice;
}

static void power_heal(void)
{
    ships[0].hp = min(ships[0].hp + randn(25) + 25, ships[0].hp_max);
}

static void power_fire_delay_down(void)
{
    ships[0].fire_delay = max(8, ships[0].fire_delay * 3 / 4);
}

static void power_fire_damage_up(void)
{
    ships[0].fire_damage = ships[0].fire_damage * 10 / 9;
}

static void power_teleport(void)
{
    ship_draw(0, true);
    ships[0].x = (randn(VGA_PWIDTH - 40) + 20) * SCALE;
    ships[0].y = (randn(VGA_PHEIGHT - 40) + 20) * SCALE;
    ships[0].dx = 0;
    ships[0].dy = 0;
}

static void power_radius_up(void)
{
    ship_draw(0, true);
    ships[0].radius = min(5, ships[0].radius + 1);
}

static void power_radius_down(void)
{
    ship_draw(0, true);
    ships[0].radius = max(1, ships[0].radius - 1);
}

static void powerup_random(int id)
{
    if (randn(ships[id].drop_rate) == 0) {
        int p = powerup_drop(ships[id].x, ships[id].y);
        if (p >= 0) {
            int select = randn(100);
            if (select < 50) {
                powerups[p].power = power_heal;
                powerups[p].color = LIGHT_GREEN;
            } else if (select < 75) {
                powerups[p].power = power_fire_delay_down;
                powerups[p].color = LIGHT_BLUE;
            } else if (select < 95) {
                powerups[p].power = power_fire_damage_up;
                powerups[p].color = LIGHT_RED;
            } else if (select < 97) {
                powerups[p].power = power_radius_up;
                powerups[p].color = LIGHT_MAGENTA;
            } else if (select < 99) {
                powerups[p].power = power_radius_down;
                powerups[p].color = WHITE;
            } else {
                powerups[p].power = power_teleport;
                powerups[p].color = YELLOW;
            }
        }
    }
}

static void print_game_over()
{
    vga_print((struct point){133, 97}, WHITE, "GAME OVER");
}

static void print_exit_help()
{
    vga_print((struct point){76, 160}, LIGHT_GRAY,
              "PRESS ANY KEY TO EXIT TO DOS");
    vga_print((struct point){100, 150}, LIGHT_GRAY,
              "HOLD FIRE TO RESTART");
}

static void print_title(bool clear)
{
    vga_print((struct point){124, 50}, clear ? BACKGROUND : LIGHT_BLUE,
              "DOS DEFENDER");
}

static void ship_check_bounds(int i)
{
    int32_t xlim = VGA_PWIDTH * SCALE;
    int32_t ylim = VGA_PHEIGHT * SCALE;
    if (ships[i].x < 0 || ships[i].x > xlim ||
        ships[i].y < 0 || ships[i].y > ylim) {
        ships[i].dx = 0;
        ships[i].dy = 0;
        ships[i].hp = 0;
    }
}

int spawn(int hp)
{
    int choice = -1;
    for (int i = 1; i < ships_max; i++) {
        if (!ships[i].hp > 0) {
            choice = i;
            break;
        }
    }
    if (choice > -1) {
        ships[choice].hp = hp;
        ships[choice].dx = ships[choice].dy = 0;
        if (randn(2)) {
            ships[choice].x = randn(2) * VGA_PWIDTH * SCALE;
            ships[choice].y = randn(VGA_PHEIGHT * SCALE);
        } else {
            ships[choice].x = randn(VGA_PWIDTH * SCALE);
            ships[choice].y = randn(2) * VGA_PHEIGHT * SCALE;
        }
    }
    return choice;
}

static void ai_player(int i)
{
    if (ships[i].hp > 0) {
        struct joystick joy;
        struct joystick_config *c = &joystick_config;
        int xrange = 2 * (c->max[0] - c->min[0]);
        int yrange = 2 * (c->max[1] - c->min[1]);
        joystick_read(&joy);
        ships[i].dx += ((joy.axis[0] - c->center[0]) * 100) / xrange;
        ships[i].dy += ((joy.axis[1] - c->center[1]) * 100) / yrange;
        rand_seed ^= joy.axis[0] - joy.axis[1]; // mix into random state
        if (joy.button[0])
            ship_fire(i);
    }
}

static void ai_dummy(int i)
{
    int den = 10;
    tick_t t = (ticks + i * 200 + i * 20) % 1000;
    int tx, ty;
    if (t < 250) {
        tx = VGA_PWIDTH  * SCALE * 1 / den;
        ty = VGA_PHEIGHT * SCALE * 1 / den;
    } else if (t < 500) {
        tx = VGA_PWIDTH  * SCALE * 1 / den;
        ty = VGA_PHEIGHT * SCALE * (den - 1) / den;
    } else if (t < 750) {
        tx = VGA_PWIDTH  * SCALE * (den - 1) / den;
        ty = VGA_PHEIGHT * SCALE * (den - 1) / den;
    } else {
        tx = VGA_PWIDTH  * SCALE * (den - 1) / den;
        ty = VGA_PHEIGHT * SCALE * 1 / den;
    }
    ships[i].dx = (tx - ships[i].x) / 200;
    ships[i].dy = (ty - ships[i].y) / 200;
    if (randn(250) == 0)
        ship_fire(i);
}

static void ai_seeker(int i)
{
    int noise = 400;
    int dx = ships[0].x - ships[i].x;
    int dy = ships[0].y - ships[i].y;
    ships[i].dx = dx / 250 + randn(noise) - noise / 2;
    ships[i].dy = dy / 250 + randn(noise) - noise / 2;
    ship_fire(i);
}

static bool ending_played = false;

static void clear()
{
    free();
    bullets = sbrk(bullets_max * sizeof(bullets[0]));
    particles = sbrk(particles_max * sizeof(particles[0]));
    ships = sbrk(ships_max * sizeof(ships[0]));
    powerups = sbrk(powerups_max * sizeof(powerups[0]));

    rand_seed += get_tick();
    ships[0] = (struct ship) {
        .x = VGA_PWIDTH / 2 * SCALE,
        .y = VGA_PHEIGHT / 2 * SCALE,
        .color_a = YELLOW,
        .color_b = LIGHT_BLUE,
        .radius = 2,
        .fire_delay = 25,
        .fire_damage = 10,
        .hp = 100,
        .hp_max = 100,
        .ai = ai_player,
        .fx_fire = &fx_fire0
    };
    ticks = 0;
    if (score > best_score)
        best_score = score;
    score = 0;
    ending_played = false;
    speaker.sample = 0;
    speaker_play(&speaker, &fx_intro_music);
    vga_clear(BACKGROUND);
}

static bool ship_exists(uint8_t color)
{
    for (int i = 1; i < ships_max; i++)
        if (ships[i].color_a == color)
            return true;
    return false;
}

int _main(void)
{
    if (!joystick_detected()) {
        print("A joystick is required to play DOS Defender!$");
        return 1;
    }

    vga_on();
    joystick_calibrate();

    /* Main Loop */
    clear();
    for (;;) {
        speaker_step(&speaker);
        if (randn(50) == 0) {
            int id = spawn(1);
            if (id > 0) {
                int select = randn(100) + ticks / 1000;
                if (select < 65) {
                    ships[id].color_a = BROWN;
                    ships[id].color_b = LIGHT_RED;
                    ships[id].radius = 2;
                    ships[id].fire_delay = 100;
                    ships[id].fire_damage = 10;
                    ships[id].drop_rate = 8;
                    ships[id].hp = 10;
                    ships[id].hp_max = 10;
                    ships[id].score = 100;
                    ships[id].ai = ai_seeker;
                    ships[id].fx_fire = &fx_fire1;
                } else if (select < 92) {
                    ships[id].color_a = GREEN;
                    ships[id].color_b = LIGHT_RED;
                    ships[id].radius = 2;
                    ships[id].fire_delay = 120;
                    ships[id].fire_damage = 10;
                    ships[id].drop_rate = 5;
                    ships[id].hp = 20;
                    ships[id].hp_max = 20;
                    ships[id].score = 125;
                    ships[id].ai = ai_dummy;
                    ships[id].fx_fire = &fx_fire1;
                } else if (select < 93) {
                    ships[id].color_a = WHITE;
                    ships[id].color_b = LIGHT_RED;
                    ships[id].radius = 1;
                    ships[id].fire_delay = 20;
                    ships[id].fire_damage = 1;
                    ships[id].drop_rate = 1;
                    ships[id].hp = 1;
                    ships[id].hp_max = 1;
                    ships[id].score = 500;
                    ships[id].ai = ai_seeker;
                    ships[id].fx_fire = &fx_fire1;
                } else if (select < 96) {
                    ships[id].color_a = RED;
                    ships[id].color_b = LIGHT_GREEN;
                    ships[id].radius = 3;
                    ships[id].fire_delay = 50;
                    ships[id].fire_damage = 25;
                    ships[id].drop_rate = 4;
                    ships[id].hp = 50;
                    ships[id].hp_max = 50;
                    ships[id].score = 250;
                    ships[id].ai = ai_seeker;
                    ships[id].fx_fire = &fx_fire2;
                } else if (select < 110) {
                    ships[id].color_a = LIGHT_MAGENTA;
                    ships[id].color_b = LIGHT_CYAN;
                    ships[id].radius = 5;
                    ships[id].fire_delay = 120;
                    ships[id].fire_damage = 50;
                    ships[id].drop_rate = 3;
                    ships[id].hp = 100;
                    ships[id].hp_max = 100;
                    ships[id].score = 1000;
                    ships[id].ai = ai_seeker;
                    ships[id].fx_fire = &fx_fire3;
                } else if (!ship_exists(LIGHT_GREEN)) {
                    ships[id].color_a = LIGHT_GREEN;
                    ships[id].color_b = YELLOW;
                    ships[id].radius = 8;
                    ships[id].fire_delay = 20;
                    ships[id].fire_damage = 90;
                    ships[id].drop_rate = 4;
                    ships[id].hp = 1000;
                    ships[id].hp_max = 1000;
                    ships[id].score = 10000;
                    ships[id].ai = ai_seeker;
                    ships[id].fx_fire = &fx_fire3;
                    speaker_play(&speaker, &fx_boss);
                } else {
                    ships[id].hp = 0;
                }
            }
        }

        if (ticks < 120) {
            print_title(false);
        } else if (ticks == 120) {
            print_title(true);
        }

        if (ships[0].hp == 0) {
            struct joystick joystick;
            joystick_read(&joystick);
            if (!ending_played) {
                speaker_play(&speaker, &fx_end_music);
                ending_played = true;
            } else if (!speaker.sample) {
                print_exit_help();
                if (joystick.button[0]) {
                    clear();
                    continue;
                }
            }
            print_game_over();
            if (kbhit())
                break;
            if (joystick.button[1]) { // restart early
                clear();
                continue;
            }
        }
        for (int i = 0; i < particles_max; i++) {
            particle_draw(i, true);
            if (particles[i].alive) {
                particle_step(i);
                if (particles[i].alive)
                    particle_draw(i, false);
            }
        }
        for (int i = 0; i < powerups_max; i++) {
            if (powerups[i].alive) {
                powerup_draw(i, true);
                powerup_step(i);
                if (powerups[i].alive)
                    powerup_draw(i, false);
            }
        }
        for (int i = 0; i < ships_max; i++) {
            if (ships[i].hp > 0 || i == 0) {
                ship_draw(i, true);
                ship_step(i);
                ships[i].ai(i);
                if (ships[i].hp > 0)
                    ship_draw(i, false);
            }
        }
        ship_check_bounds(0);
        for (int i = 0; i < bullets_max; i++) {
            bullet_draw(i, true);
            if (bullets[i].alive) {
                bullet_step(i);
                if (bullets[i].alive)
                    bullet_draw(i, false);
            }
        }
        vga_vsync();
        ticks++;
    }
    if (score > best_score)
        best_score = score;
    vga_off();
    tone_off();
    print("best score: $");
    printl(best_score);
    return 0;
}
